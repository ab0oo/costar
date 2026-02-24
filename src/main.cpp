#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>
#include <string>

#include "AppConfig.h"
#include "RuntimeGeo.h"
#include "RuntimeSettings.h"
#include "core/BootCommon.h"
#include "core/DisplayManager.h"
#include "core/TextEntry.h"
#include "core/TimeSync.h"
#include "core/TouchMapper.h"
#include "core/WifiProvisioner.h"
#include "platform/Fs.h"
#include "platform/Net.h"
#include "platform/Prefs.h"
#include "platform/Platform.h"
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
String setupGeoSource = "unknown";
int activeLayoutProfile = 0;
String activeLayoutPath = AppConfig::kLayoutPathA;
bool userBtnRawPressed = false;
bool userBtnStablePressed = false;
uint32_t userBtnLastChangeMs = 0;
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
constexpr char kLayoutPrefsNs[] = "layout";
constexpr char kLayoutProfileKey[] = "profile";
constexpr uint16_t kAdsbRadiusOptions[] = {20, 40, 80, 120};

boot::BaselineState gBaselineState;

void baselineMark(const char* stage) {
  boot::mark(gBaselineState, stage, AppConfig::kBaselineMetricsEnabled);
}

void baselineLoopMark(uint32_t nowMs) {
  (void)nowMs;
  boot::markLoop(gBaselineState, wifiReady, AppConfig::kBaselineMetricsEnabled,
                 AppConfig::kBaselineLoopLogPeriodMs);
}

String layoutPathForProfile(int profile) {
  return profile == 1 ? String(AppConfig::kLayoutPathB) : String(AppConfig::kLayoutPathA);
}

int sanitizeLayoutProfile(int profile) {
  return profile == 1 ? 1 : 0;
}

void saveLayoutProfile(int profile) {
  platform::prefs::putInt(kLayoutPrefsNs, kLayoutProfileKey, sanitizeLayoutProfile(profile));
}

void loadLayoutProfile() {
  const int savedProfile = platform::prefs::getInt(kLayoutPrefsNs, kLayoutProfileKey, 0);
  activeLayoutProfile = sanitizeLayoutProfile(savedProfile);
  activeLayoutPath = layoutPathForProfile(activeLayoutProfile);
  displayManager.setLayoutPath(activeLayoutPath);
  platform::logi("layout", "profile=%d path=%s", activeLayoutProfile, activeLayoutPath.c_str());
}

bool readUserButtonPressed() {
  if (AppConfig::kUserButtonPin < 0) {
    return false;
  }
  const int raw = digitalRead(AppConfig::kUserButtonPin);
  if (AppConfig::kUserButtonActiveLow) {
    return raw == LOW;
  }
  return raw == HIGH;
}

void initUserButton() {
  if (AppConfig::kUserButtonPin < 0) {
    return;
  }
  if (AppConfig::kUserButtonActiveLow) {
    pinMode(AppConfig::kUserButtonPin, INPUT_PULLUP);
  } else {
    pinMode(AppConfig::kUserButtonPin, INPUT);
  }
  userBtnRawPressed = readUserButtonPressed();
  userBtnStablePressed = userBtnRawPressed;
  userBtnLastChangeMs = platform::millisMs();
  platform::logi("layout", "USER button gpio=%d ready (active_%s)", AppConfig::kUserButtonPin,
                AppConfig::kUserButtonActiveLow ? "low" : "high");
}

void toggleLayoutProfile() {
  const int oldProfile = activeLayoutProfile;
  const String oldPath = activeLayoutPath;

  const int nextProfile = (activeLayoutProfile == 0) ? 1 : 0;
  const String nextPath = layoutPathForProfile(nextProfile);

  if (!displayManager.reloadLayout(nextPath)) {
    platform::logi("layout", "switch failed profile=%d path=%s, restoring profile=%d path=%s",
                  nextProfile, nextPath.c_str(), oldProfile, oldPath.c_str());
    displayManager.reloadLayout(oldPath);
    return;
  }

  activeLayoutProfile = nextProfile;
  activeLayoutPath = nextPath;
  saveLayoutProfile(activeLayoutProfile);
  platform::logi("layout", "switched profile=%d path=%s", activeLayoutProfile,
                activeLayoutPath.c_str());
}

void updateUserButton(uint32_t nowMs) {
  if (AppConfig::kUserButtonPin < 0) {
    return;
  }

  const bool pressedNow = readUserButtonPressed();
  if (pressedNow != userBtnRawPressed) {
    userBtnRawPressed = pressedNow;
    userBtnLastChangeMs = nowMs;
  }

  if (pressedNow == userBtnStablePressed) {
    return;
  }
  if (nowMs - userBtnLastChangeMs < AppConfig::kUserButtonDebounceMs) {
    return;
  }

  userBtnStablePressed = pressedNow;
  if (userBtnStablePressed) {
    toggleLayoutProfile();
  }
}

String compactGeoSource(const String& source) {
  if (source.isEmpty()) {
    return "unknown";
  }
  if (source == "manual") {
    return "manual";
  }
  if (source == "nvs-cache") {
    return "cache";
  }
  if (source == "none") {
    return "none";
  }
  if (!source.startsWith("http")) {
    return source;
  }

  int start = source.indexOf("://");
  start = (start >= 0) ? (start + 3) : 0;
  int end = source.indexOf('/', start);
  if (end < 0) {
    end = source.length();
  }
  String host = source.substring(start, end);
  if (host.startsWith("www.")) {
    host = host.substring(4);
  }
  return host;
}

void waitForTouchRelease() {
  if (!AppConfig::kTouchEnabled) {
    return;
  }
  while (touch.touched()) {
    platform::sleepMs(20);
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
  bool hasSetting = platform::prefs::getBool(kDisplayPrefsNs, kColorSetKey, false);
  bool hasInvertSetting = platform::prefs::getBool(kDisplayPrefsNs, kInvertSetKey, false);
  bool useBgr = platform::prefs::getBool(kDisplayPrefsNs, kColorBgrKey, false);
  bool useInvert = platform::prefs::getBool(kDisplayPrefsNs, kInvertOnKey, false);

  if (hasSetting && hasInvertSetting) {
    applyPanelColorOrder(useBgr);
    applyPanelInversion(useInvert);
    platform::logi("display", "from NVS: order=%s invert=%s", useBgr ? "BGR" : "RGB",
                  useInvert ? "ON" : "OFF");
    return;
  }

  if (!AppConfig::kTouchEnabled) {
    applyPanelColorOrder(useBgr);
    applyPanelInversion(useInvert);
    platform::prefs::putBool(kDisplayPrefsNs, kColorBgrKey, useBgr);
    platform::prefs::putBool(kDisplayPrefsNs, kColorSetKey, true);
    platform::prefs::putBool(kDisplayPrefsNs, kInvertOnKey, useInvert);
    platform::prefs::putBool(kDisplayPrefsNs, kInvertSetKey, true);
    platform::logi("display", "no touch; defaults order=%s invert=%s",
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
      platform::sleepMs(20);
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
      platform::prefs::putBool(kDisplayPrefsNs, kColorBgrKey, useBgr);
      platform::prefs::putBool(kDisplayPrefsNs, kColorSetKey, true);
      platform::prefs::putBool(kDisplayPrefsNs, kInvertOnKey, useInvert);
      platform::prefs::putBool(kDisplayPrefsNs, kInvertSetKey, true);
      waitForTouchRelease();
      platform::logi("display", "calibrated: order=%s invert=%s", useBgr ? "BGR" : "RGB",
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
  const int btnW = 148;
  const int btnH = 34;
  const int xL = 8;
  const int xR = 164;
  const int y1 = 34;
  const int y2 = 72;
  const int y3 = 110;
  const int y4 = 148;
  const int y5 = 186;

  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, AppConfig::kScreenWidth, 28, hdrBg);
  tft.setTextColor(TFT_WHITE, hdrBg);
  tft.drawString("Setup / Config", 8, 8, 2);
  tft.setTextColor(TFT_LIGHTGREY, hdrBg);
  tft.drawRightString(String("Geo: ") + compactGeoSource(setupGeoSource),
                      AppConfig::kScreenWidth - 8, 10, 1);

  auto drawBtn = [&](int x, int y, int w, int h, const String& label, bool alt) {
    tft.fillRoundRect(x, y, w, h, 6, alt ? btnBgAlt : btnBg);
    tft.setTextColor(TFT_WHITE, alt ? btnBgAlt : btnBg);
    tft.drawCentreString(label, x + w / 2, y + 10, 2);
  };

  drawBtn(xL, y1, btnW, btnH, "Display Cal", false);
  drawBtn(xR, y1, btnW, btnH, "WiFi Setup", false);
  drawBtn(xL, y2, btnW, btnH,
          String("Clock: ") + (RuntimeSettings::use24HourClock ? "24h" : "12h"), true);
  drawBtn(xR, y2, btnW, btnH, String("Temp: ") + (RuntimeSettings::useFahrenheit ? "F" : "C"),
          true);
  drawBtn(xL, y3, btnW, btnH, String("Distance: ") + (RuntimeSettings::useMiles ? "mi" : "km"),
          true);
  drawBtn(xR, y3, btnW, btnH, String("ADS-B: ") + RuntimeSettings::adsbRadiusNm + "nm", true);
  drawBtn(xL, y4, btnW, btnH, "City / Locale", false);
  drawBtn(xR, y4, btnW, btnH, "Lat / Lon", false);
  drawBtn(xL, y5, btnW, btnH, "Use Geo-IP", false);
  drawBtn(xR, y5, btnW, btnH, "Exit", false);
}

void runSetupScreen() {
  auto showMessage = [&](const String& line1, const String& line2, uint32_t timeoutMs = 0) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString(line1, AppConfig::kScreenWidth / 2, AppConfig::kScreenHeight / 2 - 14, 2);
    if (!line2.isEmpty()) {
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.drawString(line2, AppConfig::kScreenWidth / 2, AppConfig::kScreenHeight / 2 + 10, 2);
    }
    tft.setTextDatum(TL_DATUM);
    if (timeoutMs == 0) {
      uint16_t x = 0;
      uint16_t y = 0;
      while (!readTouchPoint(x, y)) {
        platform::sleepMs(20);
      }
      waitForTouchRelease();
    } else {
      const uint32_t startMs = platform::millisMs();
      while (platform::millisMs() - startMs < timeoutMs) {
        platform::sleepMs(20);
      }
    }
  };

  for (;;) {
    drawSetupScreen();
    uint16_t x = 0;
    uint16_t y = 0;
    while (!readTouchPoint(x, y)) {
      platform::sleepMs(20);
    }
    waitForTouchRelease();

    // Display calibration
    if (x >= 8 && x <= 156 && y >= 34 && y <= 68) {
      ensureDisplayColorOrder();
      continue;
    }

    // WiFi setup
    if (x >= 164 && x <= 312 && y >= 34 && y <= 68) {
      WifiProvisioner provisioner(tft, touch);
      wifiReady = provisioner.connectOrProvision();
      GeoIpService geo;
      if (geo.loadOverride()) {
        setupGeoSource = geo.lastSource();
      } else {
        if (geo.loadCached()) {
          setupGeoSource = geo.lastSource();
        }
        if (wifiReady) {
          if (geo.refreshFromInternet()) {
            setupGeoSource = geo.lastSource();
          }
        }
        timesync::logUiTimeContext(RuntimeGeo::timezone.c_str(), RuntimeGeo::utcOffsetMinutes,
                                   RuntimeGeo::hasUtcOffset);
      }
      continue;
    }

    // Clock 12h/24h
    if (x >= 8 && x <= 156 && y >= 72 && y <= 106) {
      RuntimeSettings::use24HourClock = !RuntimeSettings::use24HourClock;
      RuntimeSettings::save();
      continue;
    }

    // Temp units
    if (x >= 164 && x <= 312 && y >= 72 && y <= 106) {
      RuntimeSettings::useFahrenheit = !RuntimeSettings::useFahrenheit;
      RuntimeSettings::save();
      continue;
    }

    // Distance units
    if (x >= 8 && x <= 156 && y >= 110 && y <= 144) {
      RuntimeSettings::useMiles = !RuntimeSettings::useMiles;
      RuntimeSettings::save();
      continue;
    }

    // ADS-B radius
    if (x >= 164 && x <= 312 && y >= 110 && y <= 144) {
      constexpr size_t kRadiusCount = sizeof(kAdsbRadiusOptions) / sizeof(kAdsbRadiusOptions[0]);
      uint16_t nextRadius = kAdsbRadiusOptions[0];
      for (size_t i = 0; i < kRadiusCount; ++i) {
        if (RuntimeSettings::adsbRadiusNm == kAdsbRadiusOptions[i]) {
          nextRadius = kAdsbRadiusOptions[(i + 1) % kRadiusCount];
          break;
        }
      }
      RuntimeSettings::adsbRadiusNm = nextRadius;
      RuntimeSettings::save();
      continue;
    }

    // City / locale
    if (x >= 8 && x <= 156 && y >= 148 && y <= 182) {
      if (!wifiReady) {
        showMessage("WiFi required", "Connect first");
        continue;
      }
      if (!timesync::ensureUtcTime()) {
        showMessage("Time sync failed", "SSL may fail");
      }
      TextEntry entry(tft, touch);
      TextEntryOptions options;
      options.title = "City / Locale";
      options.subtitle = "ex: Seattle, WA";
      const String city = entry.prompt(options);
      if (city == "__CANCEL__" || city.isEmpty()) {
        continue;
      }
      GeoIpService geo;
      if (geo.setManualCity(city)) {
        setupGeoSource = geo.lastSource();
        timesync::logUiTimeContext(RuntimeGeo::timezone.c_str(), RuntimeGeo::utcOffsetMinutes,
                                   RuntimeGeo::hasUtcOffset);
        showMessage("Location set", city, 1200);
      } else {
        showMessage("Location failed", geo.lastError());
      }
      continue;
    }

    // Lat / Lon
    if (x >= 164 && x <= 312 && y >= 148 && y <= 182) {
      if (!wifiReady) {
        showMessage("WiFi required", "Connect first");
        continue;
      }
      if (!timesync::ensureUtcTime()) {
        showMessage("Time sync failed", "SSL may fail");
      }
      TextEntry entry(tft, touch);
      TextEntryOptions latOpt;
      latOpt.title = "Latitude";
      latOpt.subtitle = "Range -90..90";
      latOpt.numericOnly = true;
      const String latText = entry.prompt(latOpt);
      if (latText == "__CANCEL__" || latText.isEmpty()) {
        continue;
      }
      TextEntryOptions lonOpt;
      lonOpt.title = "Longitude";
      lonOpt.subtitle = "Range -180..180";
      lonOpt.numericOnly = true;
      const String lonText = entry.prompt(lonOpt);
      if (lonText == "__CANCEL__" || lonText.isEmpty()) {
        continue;
      }
      const float lat = latText.toFloat();
      const float lon = lonText.toFloat();
      if (lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f) {
        showMessage("Invalid coords", "Check range");
        continue;
      }
      GeoIpService geo;
      if (geo.setManualLatLon(lat, lon)) {
        setupGeoSource = geo.lastSource();
        timesync::logUiTimeContext(RuntimeGeo::timezone.c_str(), RuntimeGeo::utcOffsetMinutes,
                                   RuntimeGeo::hasUtcOffset);
        showMessage("Location set", String(lat, 4) + "," + String(lon, 4), 1200);
      } else {
        showMessage("Location failed", geo.lastError());
      }
      continue;
    }

    // Use Geo-IP
    if (x >= 8 && x <= 156 && y >= 186 && y <= 220) {
      GeoIpService geo;
      geo.clearOverride();
      if (geo.loadCached()) {
        setupGeoSource = geo.lastSource();
      }
      if (wifiReady) {
        if (geo.refreshFromInternet()) {
          setupGeoSource = geo.lastSource();
        }
      }
      timesync::logUiTimeContext(RuntimeGeo::timezone.c_str(), RuntimeGeo::utcOffsetMinutes,
                                 RuntimeGeo::hasUtcOffset);
      showMessage("Geo-IP enabled", setupGeoSource, 1000);
      continue;
    }

    // Exit
    if (x >= 164 && x <= 312 && y >= 186 && y <= 220) {
      return;
    }
  }
}

void initBacklight() {
  if (AppConfig::kBacklightPin < 0) {
    platform::logi("boot", "backlight pin control disabled");
    return;
  }
  pinMode(AppConfig::kBacklightPin, OUTPUT);
  digitalWrite(AppConfig::kBacklightPin, AppConfig::kBacklightOnHigh ? HIGH : LOW);
  platform::logi("boot", "backlight pin %d set %s", AppConfig::kBacklightPin,
                AppConfig::kBacklightOnHigh ? "HIGH" : "LOW");
}

void initBoardLedOff() {
  if (AppConfig::kBoardBlueLedPin < 0) {
    return;
  }
  pinMode(AppConfig::kBoardBlueLedPin, OUTPUT);
  digitalWrite(AppConfig::kBoardBlueLedPin, AppConfig::kBoardBlueLedOffHigh ? HIGH : LOW);
  platform::logi("boot", "board LED pin %d forced %s", AppConfig::kBoardBlueLedPin,
                AppConfig::kBoardBlueLedOffHigh ? "HIGH" : "LOW");
}

void initSpiDeviceChipSelects() {
  // Keep non-display SPI devices deselected to prevent bus contention.
  if (AppConfig::kTouchCsPin >= 0) {
    pinMode(AppConfig::kTouchCsPin, OUTPUT);
    digitalWrite(AppConfig::kTouchCsPin, HIGH);
    platform::logi("boot", "touch CS %d forced HIGH", AppConfig::kTouchCsPin);
  }

  if (AppConfig::kSdCsPin >= 0) {
    pinMode(AppConfig::kSdCsPin, OUTPUT);
    digitalWrite(AppConfig::kSdCsPin, HIGH);
    platform::logi("boot", "SD CS %d forced HIGH", AppConfig::kSdCsPin);
  }
}

void initTouch() {
  if (!AppConfig::kTouchEnabled) {
    platform::logi("boot", "touch disabled by config");
    return;
  }
  touchSpi.begin(AppConfig::kTouchSpiSckPin, AppConfig::kTouchSpiMisoPin,
                 AppConfig::kTouchSpiMosiPin, AppConfig::kTouchCsPin);
  touch.begin(touchSpi);
  touch.setRotation(2);
  platform::logi("boot", "touch initialized on VSPI");
}

void initDiagnosticLed() {
  if (AppConfig::kDiagnosticLedPin < 0) {
    return;
  }
  pinMode(AppConfig::kDiagnosticLedPin, OUTPUT);
  digitalWrite(AppConfig::kDiagnosticLedPin, LOW);
}

void runDiagnosticLoop() {
  const uint32_t nowMs = platform::millisMs();
  if (nowMs - diagnosticLastBlinkMs >= AppConfig::kDiagnosticBlinkMs) {
    diagnosticLastBlinkMs = nowMs;
    diagnosticLedState = !diagnosticLedState;
    if (AppConfig::kDiagnosticLedPin >= 0) {
      digitalWrite(AppConfig::kDiagnosticLedPin, diagnosticLedState ? HIGH : LOW);
    }
    platform::logi("diag", "heartbeat ms=%lu, led=%d", static_cast<unsigned long>(nowMs),
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
  const uint32_t nowMs = platform::millisMs();
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
    platform::logi("tftdiag", "alive ms=%lu x=%d", static_cast<unsigned long>(nowMs), tftDiagX);
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

}  // namespace

void setup() {
  platform::serialBegin(115200);
  platform::sleepMs(600);
  boot::start(gBaselineState);
  platform::logi("boot", "WidgetOS boot");
  platform::logi("boot", "setup start");
  baselineMark("setup_start");
  initUserButton();
  loadLayoutProfile();
  initBoardLedOff();

  if (!platform::fs::begin(true)) {
    platform::loge("boot", "LittleFS mount failed");
  } else {
    platform::logi("boot", "LittleFS mounted");
  }
  baselineMark("littlefs_ready");
  RuntimeSettings::load();
  boot::logSettingsSummary(false);

  if (diagnosticMode) {
    platform::logi("diag", "Diagnostic mode enabled; skipping TFT/touch/WiFi");
    initDiagnosticLed();
    platform::logi("diag", "Expect serial heartbeat + LED blink");
    return;
  }

  platform::logi("boot", "init backlight + TFT");
  initSpiDeviceChipSelects();
  initBacklight();
  tft.init();
  tft.setRotation(AppConfig::kRotation);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Booting...", 8, 8, 2);
  baselineMark("tft_ready");

  if (tftOnlyDiagnosticMode) {
    platform::logi("tftdiag", "TFT-only diagnostic mode enabled");
    drawTftDiagnosticScreen();
    return;
  }

  if (staticFrameMode) {
    platform::logi("static", "static-frame mode enabled");
    drawStaticFrame();
    return;
  }

  platform::logi("boot", "init touch");
  initTouch();

  platform::logi("boot", "color-order calibration");
  ensureDisplayColorOrder();

  platform::logi("boot", "start wifi provisioning");
  WifiProvisioner provisioner(tft, touch);
  wifiReady = provisioner.connectOrProvision();
  baselineMark("wifi_ready");

  GeoIpService geo;
  if (geo.loadOverride()) {
    setupGeoSource = geo.lastSource();
    platform::logi("geo", "manual source=%s lat=%.4f lon=%.4f tz=%s off_min=%d known=%d",
                  geo.lastSource().c_str(), RuntimeGeo::latitude, RuntimeGeo::longitude,
                  RuntimeGeo::timezone.c_str(), RuntimeGeo::utcOffsetMinutes,
                  RuntimeGeo::hasUtcOffset ? 1 : 0);
  } else if (geo.loadCached()) {
    setupGeoSource = geo.lastSource();
    platform::logi("geo", "cache hit source=%s lat=%.4f lon=%.4f tz=%s off_min=%d known=%d",
                  geo.lastSource().c_str(), RuntimeGeo::latitude, RuntimeGeo::longitude,
                  RuntimeGeo::timezone.c_str(), RuntimeGeo::utcOffsetMinutes,
                  RuntimeGeo::hasUtcOffset ? 1 : 0);
  } else {
    setupGeoSource = "none";
    platform::logw("geo", "cache miss: %s", geo.lastError().c_str());
  }

  if (wifiReady && geo.lastSource() != "manual") {
    std::string ipText;
    if (platform::net::getLocalIp(ipText)) {
      platform::logi("wifi", "connected ip=%s", ipText.c_str());
    } else {
      platform::logi("wifi", "connected");
    }
    if (geo.refreshFromInternet()) {
      setupGeoSource = geo.lastSource();
      platform::logi("geo", "online source=%s lat=%.4f lon=%.4f tz=%s off_min=%d known=%d",
                    geo.lastSource().c_str(), RuntimeGeo::latitude, RuntimeGeo::longitude,
                    RuntimeGeo::timezone.c_str(), RuntimeGeo::utcOffsetMinutes,
                    RuntimeGeo::hasUtcOffset ? 1 : 0);
    } else {
      platform::logw("geo", "online fetch failed source=%s reason=%s",
                    geo.lastSource().c_str(), geo.lastError().c_str());
    }
    timesync::logUiTimeContext(RuntimeGeo::timezone.c_str(), RuntimeGeo::utcOffsetMinutes,
                               RuntimeGeo::hasUtcOffset);
  } else {
    platform::logi("boot", "continuing offline");
    timesync::logUiTimeContext(RuntimeGeo::timezone.c_str(), RuntimeGeo::utcOffsetMinutes,
                               RuntimeGeo::hasUtcOffset);
  }
  baselineMark("geo_time_ready");

  platform::logi("boot", "init display manager");
  if (!displayManager.begin()) {
    platform::loge("boot", "display manager failed to load layout");
  }
  baselineMark("display_ready");

  if (wifiReady) {
    platform::logi("boot", "live data widgets enabled");
  }
  platform::logi("boot", "setup complete");
  baselineMark("setup_complete");
}

void loop() {
  if (diagnosticMode) {
    runDiagnosticLoop();
    platform::sleepMs(10);
    return;
  }

  if (tftOnlyDiagnosticMode) {
    runTftDiagnosticLoop();
    platform::sleepMs(5);
    return;
  }

  if (staticFrameMode) {
    platform::sleepMs(50);
    return;
  }

  const uint32_t nowMs = platform::millisMs();
  baselineLoopMark(nowMs);
  updateUserButton(nowMs);

  if (AppConfig::kTouchEnabled && touch.touched()) {
    TouchPoint point;
    if (TouchMapper::mapRaw(touch.getPoint(), point)) {
      const bool inCorner = (point.x >= static_cast<int16_t>(AppConfig::kScreenWidth - 32) &&
                             point.y <= 24);
      if (inCorner) {
        const uint32_t startMs = platform::millisMs();
        bool held = false;
        while (touch.touched()) {
          TouchPoint current;
          if (!TouchMapper::mapRaw(touch.getPoint(), current)) {
            break;
          }
          if (current.x < static_cast<int16_t>(AppConfig::kScreenWidth - 32) ||
              current.y > 24) {
            break;
          }
          if (platform::millisMs() - startMs > 650) {
            held = true;
            break;
          }
          platform::sleepMs(20);
        }
        if (held) {
          waitForTouchRelease();
          runSetupScreen();
          displayManager.reloadLayout();
        }
      } else {
        displayManager.onTouch(point.x, point.y);
        waitForTouchRelease();
      }
    }
  }

  displayManager.loop(nowMs);
  platform::sleepMs(AppConfig::kLoopDelayMs);
}
