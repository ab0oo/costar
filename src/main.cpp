#include <Arduino.h>
#include <SPI.h>
#include <SPIFFS.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>
#include <Preferences.h>

#include "AppConfig.h"
#include "RuntimeGeo.h"
#include "RuntimeSettings.h"
#include "core/DisplayManager.h"
#include "core/TouchMapper.h"
#include "core/WifiProvisioner.h"
#include "services/GeoIpService.h"

TFT_eSPI tft = TFT_eSPI();
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
SPIClass touchSpi(VSPI);
DisplayManager displayManager(tft, AppConfig::kDefaultLayoutPath);

namespace {
bool wifiReady = false;
bool diagnosticMode = AppConfig::kDiagnosticMode;
bool tftOnlyDiagnosticMode = AppConfig::kTftOnlyDiagnosticMode;
bool staticFrameMode = AppConfig::kStaticFrameMode;
uint32_t diagnosticLastBlinkMs = 0;
bool diagnosticLedState = false;
uint32_t tftDiagLastMs = 0;
int16_t tftDiagX = 0;
int8_t tftDiagDir = 1;
constexpr char kDisplayPrefsNs[] = "display";
constexpr char kColorSetKey[] = "color_set";
constexpr char kColorBgrKey[] = "color_bgr";
constexpr char kInvertSetKey[] = "inv_set";
constexpr char kInvertOnKey[] = "inv_on";
void configureTimeFromGeo();

void waitForTouchRelease() {
  if (!AppConfig::kTouchEnabled) {
    return;
  }
  while (touch.touched()) {
    delay(20);
  }
}

bool readTouchPoint(uint16_t& x, uint16_t& y) {
  if (!AppConfig::kTouchEnabled || !touch.touched()) {
    return false;
  }
  TouchPoint point;
  if (!TouchMapper::mapRaw(touch.getPoint(), point)) {
    return false;
  }
  x = point.x;
  y = point.y;
  return true;
}

void applyPanelColorOrder(bool bgr) {
  const uint8_t rot = AppConfig::kRotation & 0x03;
  uint8_t madctl = 0x00;
  switch (rot) {
    case 0:
      madctl = 0x40;  // MX
      break;
    case 1:
      madctl = 0x20;  // MV
      break;
    case 2:
      madctl = 0x80;  // MY
      break;
    case 3:
      madctl = 0xE0;  // MX|MY|MV
      break;
  }
  if (bgr) {
    madctl |= 0x08;  // BGR
  }

  tft.startWrite();
  tft.writecommand(0x36);  // MADCTL
  tft.writedata(madctl);
  tft.endWrite();
}

void applyPanelInversion(bool inverted) { tft.invertDisplay(inverted); }

void drawColorCalibrationScreen(bool bgr, bool inverted) {
  const uint16_t hdrBg = tft.color565(15, 20, 30);
  const uint16_t swapBg = tft.color565(110, 70, 20);
  const uint16_t invBg = tft.color565(70, 40, 120);
  const uint16_t okBg = tft.color565(20, 110, 45);

  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, AppConfig::kScreenWidth, 28, hdrBg);
  tft.setTextColor(TFT_WHITE, hdrBg);
  tft.drawString("Display Color Calibration", 8, 8, 2);

  tft.fillRect(12, 40, 96, 52, TFT_RED);
  tft.fillRect(112, 40, 96, 52, TFT_GREEN);
  tft.fillRect(212, 40, 96, 52, TFT_BLUE);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Expected: RED  GREEN  BLUE", 14, 102, 2);
  tft.drawString("If colors are wrong, tap Swap R/B.", 14, 122, 2);

  tft.fillRoundRect(8, 172, 100, 48, 6, swapBg);
  tft.setTextColor(TFT_WHITE, swapBg);
  tft.drawCentreString("Swap R/B", 58, 186, 2);

  tft.fillRoundRect(110, 172, 100, 48, 6, invBg);
  tft.setTextColor(TFT_WHITE, invBg);
  tft.drawCentreString("Invert", 160, 186, 2);

  tft.fillRoundRect(212, 172, 100, 48, 6, okBg);
  tft.setTextColor(TFT_WHITE, okBg);
  tft.drawCentreString("Looks OK", 262, 186, 2);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(String("Order: ") + (bgr ? "BGR" : "RGB"), 12, 146, 2);
  tft.drawString(String("Invert: ") + (inverted ? "ON" : "OFF"), 160, 146, 2);
}

void ensureDisplayColorOrder() {
  Preferences prefs;
  prefs.begin(kDisplayPrefsNs, false);
  bool hasSetting = prefs.getBool(kColorSetKey, false);
  bool hasInvertSetting = prefs.getBool(kInvertSetKey, false);
  bool useBgr = prefs.getBool(kColorBgrKey, false);
  bool useInvert = prefs.getBool(kInvertOnKey, false);

  if (hasSetting && hasInvertSetting) {
    applyPanelColorOrder(useBgr);
    applyPanelInversion(useInvert);
    Serial.printf("[display] from NVS: order=%s invert=%s\n", useBgr ? "BGR" : "RGB",
                  useInvert ? "ON" : "OFF");
    prefs.end();
    return;
  }

  if (!AppConfig::kTouchEnabled) {
    applyPanelColorOrder(useBgr);
    applyPanelInversion(useInvert);
    prefs.putBool(kColorBgrKey, useBgr);
    prefs.putBool(kColorSetKey, true);
    prefs.putBool(kInvertOnKey, useInvert);
    prefs.putBool(kInvertSetKey, true);
    prefs.end();
    Serial.printf("[display] no touch; defaults order=%s invert=%s\n",
                  useBgr ? "BGR" : "RGB", useInvert ? "ON" : "OFF");
    return;
  }

  applyPanelColorOrder(useBgr);
  applyPanelInversion(useInvert);
  for (;;) {
    drawColorCalibrationScreen(useBgr, useInvert);
    uint16_t x = 0;
    uint16_t y = 0;
    while (!readTouchPoint(x, y)) {
      delay(20);
    }

    if (y >= 172 && y <= 220 && x >= 8 && x <= 108) {
      useBgr = !useBgr;
      applyPanelColorOrder(useBgr);
      waitForTouchRelease();
      continue;
    }

    if (y >= 172 && y <= 220 && x >= 110 && x <= 210) {
      useInvert = !useInvert;
      applyPanelInversion(useInvert);
      waitForTouchRelease();
      continue;
    }

    if (y >= 172 && y <= 220 && x >= 212 && x <= 312) {
      prefs.putBool(kColorBgrKey, useBgr);
      prefs.putBool(kColorSetKey, true);
      prefs.putBool(kInvertOnKey, useInvert);
      prefs.putBool(kInvertSetKey, true);
      prefs.end();
      waitForTouchRelease();
      Serial.printf("[display] calibrated: order=%s invert=%s\n", useBgr ? "BGR" : "RGB",
                    useInvert ? "ON" : "OFF");
      return;
    }
    waitForTouchRelease();
  }
}

void drawSetupScreen() {
  const uint16_t hdrBg = tft.color565(20, 24, 36);
  const uint16_t btnBg = tft.color565(36, 42, 60);
  const uint16_t btnBgAlt = tft.color565(32, 58, 44);

  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, AppConfig::kScreenWidth, 28, hdrBg);
  tft.setTextColor(TFT_WHITE, hdrBg);
  tft.drawString("Setup / Config", 8, 8, 2);

  auto drawBtn = [&](int x, int y, int w, int h, const String& label, bool alt) {
    tft.fillRoundRect(x, y, w, h, 6, alt ? btnBgAlt : btnBg);
    tft.setTextColor(TFT_WHITE, alt ? btnBgAlt : btnBg);
    tft.drawCentreString(label, x + w / 2, y + 10, 2);
  };

  drawBtn(8, 36, 148, 42, "Display Cal", false);
  drawBtn(164, 36, 148, 42, "WiFi Setup", false);
  drawBtn(8, 86, 148, 42, String("Clock: ") + (RuntimeSettings::use24HourClock ? "24h" : "12h"),
          true);
  drawBtn(164, 86, 148, 42, String("Temp: ") + (RuntimeSettings::useFahrenheit ? "F" : "C"),
          true);
  drawBtn(8, 136, 148, 42, String("Distance: ") + (RuntimeSettings::useMiles ? "mi" : "km"),
          true);
  drawBtn(164, 136, 148, 42, "Exit", false);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Open setup: tap top-right corner in dashboard", 8, 198, 1);
}

void runSetupScreen() {
  for (;;) {
    drawSetupScreen();
    uint16_t x = 0;
    uint16_t y = 0;
    while (!readTouchPoint(x, y)) {
      delay(20);
    }
    waitForTouchRelease();

    // Display calibration
    if (x >= 8 && x <= 156 && y >= 36 && y <= 78) {
      ensureDisplayColorOrder();
      continue;
    }

    // WiFi setup
    if (x >= 164 && x <= 312 && y >= 36 && y <= 78) {
      WifiProvisioner provisioner(tft, touch);
      wifiReady = provisioner.connectOrProvision();
      GeoIpService geo;
      geo.loadCached();
      if (wifiReady) {
        geo.refreshFromInternet();
      }
      configureTimeFromGeo();
      continue;
    }

    // Clock 12h/24h
    if (x >= 8 && x <= 156 && y >= 86 && y <= 128) {
      RuntimeSettings::use24HourClock = !RuntimeSettings::use24HourClock;
      RuntimeSettings::save();
      continue;
    }

    // Temp units
    if (x >= 164 && x <= 312 && y >= 86 && y <= 128) {
      RuntimeSettings::useFahrenheit = !RuntimeSettings::useFahrenheit;
      RuntimeSettings::save();
      continue;
    }

    // Distance units
    if (x >= 8 && x <= 156 && y >= 136 && y <= 178) {
      RuntimeSettings::useMiles = !RuntimeSettings::useMiles;
      RuntimeSettings::save();
      continue;
    }

    // Exit
    if (x >= 164 && x <= 312 && y >= 136 && y <= 178) {
      return;
    }
  }
}

void initBacklight() {
  if (AppConfig::kBacklightPin < 0) {
    Serial.println("[boot] backlight pin control disabled");
    return;
  }
  pinMode(AppConfig::kBacklightPin, OUTPUT);
  digitalWrite(AppConfig::kBacklightPin, AppConfig::kBacklightOnHigh ? HIGH : LOW);
  Serial.printf("[boot] backlight pin %d set %s\n", AppConfig::kBacklightPin,
                AppConfig::kBacklightOnHigh ? "HIGH" : "LOW");
}

void initSpiDeviceChipSelects() {
  // Keep non-display SPI devices deselected to prevent bus contention.
  if (AppConfig::kTouchCsPin >= 0) {
    pinMode(AppConfig::kTouchCsPin, OUTPUT);
    digitalWrite(AppConfig::kTouchCsPin, HIGH);
    Serial.printf("[boot] touch CS %d forced HIGH\n", AppConfig::kTouchCsPin);
  }

  if (AppConfig::kSdCsPin >= 0) {
    pinMode(AppConfig::kSdCsPin, OUTPUT);
    digitalWrite(AppConfig::kSdCsPin, HIGH);
    Serial.printf("[boot] SD CS %d forced HIGH\n", AppConfig::kSdCsPin);
  }
}

void initTouch() {
  if (!AppConfig::kTouchEnabled) {
    Serial.println("[boot] touch disabled by config");
    return;
  }
  touchSpi.begin(AppConfig::kTouchSpiSckPin, AppConfig::kTouchSpiMisoPin,
                 AppConfig::kTouchSpiMosiPin, AppConfig::kTouchCsPin);
  touch.begin(touchSpi);
  touch.setRotation(2);
  Serial.println("[boot] touch initialized on VSPI");
}

void initDiagnosticLed() {
  if (AppConfig::kDiagnosticLedPin < 0) {
    return;
  }
  pinMode(AppConfig::kDiagnosticLedPin, OUTPUT);
  digitalWrite(AppConfig::kDiagnosticLedPin, LOW);
}

void runDiagnosticLoop() {
  const uint32_t nowMs = millis();
  if (nowMs - diagnosticLastBlinkMs >= AppConfig::kDiagnosticBlinkMs) {
    diagnosticLastBlinkMs = nowMs;
    diagnosticLedState = !diagnosticLedState;
    if (AppConfig::kDiagnosticLedPin >= 0) {
      digitalWrite(AppConfig::kDiagnosticLedPin, diagnosticLedState ? HIGH : LOW);
    }
    Serial.printf("[diag] heartbeat ms=%lu, led=%d\n", static_cast<unsigned long>(nowMs),
                  diagnosticLedState ? 1 : 0);
  }
}

void drawTftDiagnosticScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, 64, 40, TFT_RED);
  tft.fillRect(64, 0, 64, 40, TFT_GREEN);
  tft.fillRect(128, 0, 64, 40, TFT_BLUE);
  tft.fillRect(192, 0, 64, 40, TFT_YELLOW);
  tft.fillRect(256, 0, 64, 40, TFT_CYAN);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("TFT DIAG MODE", 8, 50, 2);
  tft.drawString("Expect moving white bar", 8, 68, 2);
  tft.drawString("No WiFi/widgets active", 8, 86, 2);
}

void runTftDiagnosticLoop() {
  const uint32_t nowMs = millis();
  if (nowMs - tftDiagLastMs < 20) {
    return;
  }
  tftDiagLastMs = nowMs;

  tft.fillRect(0, 120, AppConfig::kScreenWidth, 10, TFT_BLACK);
  tft.fillRect(tftDiagX, 120, 30, 10, TFT_WHITE);
  tftDiagX += tftDiagDir * 3;
  if (tftDiagX <= 0 || tftDiagX >= static_cast<int16_t>(AppConfig::kScreenWidth - 30)) {
    tftDiagDir = -tftDiagDir;
  }

  if ((nowMs % 1000) < 25) {
    Serial.printf("[tftdiag] alive ms=%lu x=%d\n", static_cast<unsigned long>(nowMs), tftDiagX);
  }
}

void drawStaticFrame() {
  tft.fillScreen(TFT_BLACK);
  tft.drawRect(0, 0, AppConfig::kScreenWidth, AppConfig::kScreenHeight, TFT_WHITE);
  tft.fillRect(0, 0, AppConfig::kScreenWidth, 34, TFT_DARKGREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("STATIC FRAME MODE", 8, 8, 2);

  tft.fillRect(12, 52, 90, 60, TFT_NAVY);
  tft.fillRect(116, 52, 90, 60, TFT_MAROON);
  tft.fillRect(220, 52, 88, 60, TFT_DARKCYAN);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawCentreString("A", 57, 78, 4);
  tft.setTextColor(TFT_WHITE, TFT_MAROON);
  tft.drawCentreString("B", 161, 78, 4);
  tft.setTextColor(TFT_WHITE, TFT_DARKCYAN);
  tft.drawCentreString("C", 264, 78, 4);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("No WiFi, no widgets, no updates", 8, 132, 2);
  tft.drawString("If this is stable, issue is runtime path", 8, 152, 2);
}

void configureTimeFromGeo() {
  // Always sync device clock in UTC; widgets apply geo/local offset explicitly.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  if (RuntimeGeo::hasUtcOffset) {
    Serial.printf("[time] NTP UTC sync; local UI offset=%d min tz='%s'\n",
                  RuntimeGeo::utcOffsetMinutes, RuntimeGeo::timezone.c_str());
  } else if (!RuntimeGeo::timezone.isEmpty()) {
    Serial.printf("[time] NTP UTC sync; tz='%s' (offset unknown)\n",
                  RuntimeGeo::timezone.c_str());
  } else {
    Serial.println("[time] NTP UTC sync; timezone unavailable");
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println();
  Serial.println("== WidgetOS boot ==");
  Serial.println("[boot] setup start");

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed");
  } else {
    Serial.println("[boot] SPIFFS mounted");
  }
  RuntimeSettings::load();
  Serial.printf("[settings] clock=%s temp=%s dist=%s\n",
                RuntimeSettings::use24HourClock ? "24h" : "12h",
                RuntimeSettings::useFahrenheit ? "F" : "C",
                RuntimeSettings::useMiles ? "mi" : "km");

  if (diagnosticMode) {
    Serial.println("[diag] Diagnostic mode enabled; skipping TFT/touch/WiFi");
    initDiagnosticLed();
    Serial.println("[diag] Expect serial heartbeat + LED blink");
    return;
  }

  Serial.println("[boot] init backlight + TFT");
  initSpiDeviceChipSelects();
  initBacklight();
  tft.init();
  tft.setRotation(AppConfig::kRotation);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Booting...", 8, 8, 2);

  if (tftOnlyDiagnosticMode) {
    Serial.println("[tftdiag] TFT-only diagnostic mode enabled");
    drawTftDiagnosticScreen();
    return;
  }

  if (staticFrameMode) {
    Serial.println("[static] static-frame mode enabled");
    drawStaticFrame();
    return;
  }

  Serial.println("[boot] init touch");
  initTouch();

  Serial.println("[boot] color-order calibration");
  ensureDisplayColorOrder();

  Serial.println("[boot] start wifi provisioning");
  WifiProvisioner provisioner(tft, touch);
  wifiReady = provisioner.connectOrProvision();

  GeoIpService geo;
  if (geo.loadCached()) {
    Serial.printf("[geo] cache hit source=%s lat=%.4f lon=%.4f tz=%s off_min=%d known=%d\n",
                  geo.lastSource().c_str(), RuntimeGeo::latitude, RuntimeGeo::longitude,
                  RuntimeGeo::timezone.c_str(), RuntimeGeo::utcOffsetMinutes,
                  RuntimeGeo::hasUtcOffset ? 1 : 0);
  } else {
    Serial.printf("[geo] cache miss: %s\n", geo.lastError().c_str());
  }

  if (wifiReady) {
    Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
    if (geo.refreshFromInternet()) {
      Serial.printf("[geo] online source=%s lat=%.4f lon=%.4f tz=%s off_min=%d known=%d\n",
                    geo.lastSource().c_str(), RuntimeGeo::latitude, RuntimeGeo::longitude,
                    RuntimeGeo::timezone.c_str(), RuntimeGeo::utcOffsetMinutes,
                    RuntimeGeo::hasUtcOffset ? 1 : 0);
    } else {
      Serial.printf("[geo] online fetch failed source=%s reason=%s\n",
                    geo.lastSource().c_str(), geo.lastError().c_str());
    }
    configureTimeFromGeo();
  } else {
    Serial.println("[boot] continuing offline");
    configureTimeFromGeo();
  }

  Serial.println("[boot] init display manager");
  if (!displayManager.begin()) {
    Serial.println("Display manager failed to load layout");
  }

  if (wifiReady) {
    Serial.println("Live data widgets enabled");
  }
  Serial.println("[boot] setup complete");
}

void loop() {
  if (diagnosticMode) {
    runDiagnosticLoop();
    delay(10);
    return;
  }

  if (tftOnlyDiagnosticMode) {
    runTftDiagnosticLoop();
    delay(5);
    return;
  }

  if (staticFrameMode) {
    delay(50);
    return;
  }

  const uint32_t nowMs = millis();

  if (AppConfig::kTouchEnabled && touch.touched()) {
    TouchPoint point;
    if (TouchMapper::mapRaw(touch.getPoint(), point)) {
      if (point.x >= static_cast<int16_t>(AppConfig::kScreenWidth - 32) && point.y <= 24) {
        waitForTouchRelease();
        runSetupScreen();
        displayManager.reloadLayout();
      } else {
        displayManager.onTouch(point.x, point.y);
      }
    }
  }

  displayManager.loop(nowMs);
  delay(AppConfig::kLoopDelayMs);
}
