#include "core/WifiProvisioner.h"

#include <algorithm>
#include <cctype>

#include "AppConfig.h"
#include "core/TouchMapper.h"

namespace {
constexpr char kPrefsNamespace[] = "wifi";
constexpr char kPrefsSsidKey[] = "ssid";
constexpr char kPrefsPassKey[] = "password";

constexpr int16_t kHeaderH = 28;
constexpr int16_t kRowH = 22;

constexpr int16_t kKW = 28;
constexpr int16_t kKH = 28;
constexpr int16_t kKG = 3;
constexpr int16_t kKS = kKW + kKG;

constexpr int16_t kKY0 = 52;
constexpr int16_t kKY1 = kKY0 + kKH + kKG;
constexpr int16_t kKY2 = kKY1 + kKH + kKG;
constexpr int16_t kKY3 = kKY2 + kKH + kKG;

constexpr int16_t kRX10 = 6;
constexpr int16_t kRX9 = 22;

constexpr int16_t kShiftX = 8;
constexpr int16_t kShiftW = 42;
constexpr int16_t kAlpha7X = kShiftX + kShiftW + kKG;
constexpr int16_t kBsX = 270;
constexpr int16_t kBsW = 42;

constexpr int16_t kModeX = 8;
constexpr int16_t kModeW = 50;
constexpr int16_t kSpaceX = kModeX + kModeW + kKG;
constexpr int16_t kEnterX = 252;
constexpr int16_t kEnterW = 60;
constexpr int16_t kSpaceW = kEnterX - kKG - kSpaceX;
}  // namespace

WifiProvisioner::WifiProvisioner(TFT_eSPI& tft, XPT2046_Touchscreen& touch)
    : tft_(tft), touch_(touch) {}

bool WifiProvisioner::connectOrProvision() {
  WiFi.mode(WIFI_STA);
  Serial.println("[wifi] station mode enabled");

  if (tryStoredCredentials()) {
    Serial.println("[wifi] connected with stored credentials");
    return true;
  }

  if (!AppConfig::kTouchEnabled) {
    Serial.println("[wifi] touch disabled; skipping interactive provisioning");
    drawStatus("WiFi unavailable", "Touch disabled");
    delay(600);
    return false;
  }

  Serial.println("[wifi] no valid stored credentials; entering provisioning UI");

  while (true) {
    std::vector<NetworkInfo> networks;
    if (!scanNetworks(networks)) {
      drawStatus("WiFi scan failed", "Tap to retry");
      uint16_t x = 0;
      uint16_t y = 0;
      while (!readTouch(x, y)) {
        delay(20);
      }
      waitForTouchRelease();
      continue;
    }

    const int selected = pickNetwork(networks);
    if (selected == -2) {
      drawStatus("WiFi skipped", "Running offline");
      delay(500);
      return false;
    }
    if (selected < 0 || selected >= static_cast<int>(networks.size())) {
      continue;
    }

    const NetworkInfo& net = networks[selected];
    Serial.printf("[wifi] selected SSID: %s (secure=%d)\n", net.ssid.c_str(),
                  net.secure ? 1 : 0);

    const String password = net.secure ? promptPassword(net.ssid) : String();
    if (password == "__CANCEL__") {
      continue;
    }

    drawStatus("Connecting...", net.ssid);
    if (tryConnect(net.ssid, password, true)) {
      drawStatus("WiFi connected", WiFi.localIP().toString());
      delay(500);
      return true;
    }

    drawStatus("Connection failed", "Tap to continue");
    uint16_t x = 0;
    uint16_t y = 0;
    while (!readTouch(x, y)) {
      delay(20);
    }
    waitForTouchRelease();
  }
}

bool WifiProvisioner::tryStoredCredentials() {
  prefs_.begin(kPrefsNamespace, true);
  const String savedSsid = prefs_.getString(kPrefsSsidKey, "");
  const String savedPass = prefs_.getString(kPrefsPassKey, "");
  prefs_.end();

  if (savedSsid.isEmpty()) {
    Serial.println("[wifi] stored SSID empty");
    return false;
  }

  drawStatus("Connecting saved WiFi", savedSsid);
  return tryConnect(savedSsid, savedPass, false);
}

bool WifiProvisioner::tryConnect(const String& ssid, const String& password,
                                 bool persist) {
  WiFi.disconnect(true);
  delay(120);
  WiFi.begin(ssid.c_str(), password.c_str());

  const uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startMs < AppConfig::kWifiConnectTimeoutMs) {
    delay(120);
  }

  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (persist) {
    persistCredentials(ssid, password);
  }
  return true;
}

bool WifiProvisioner::scanNetworks(std::vector<NetworkInfo>& networks) {
  networks.clear();
  drawStatus("Scanning for WiFi", "Please wait");

  WiFi.scanDelete();
  const int count = WiFi.scanNetworks(false, true);
  if (count < 0) {
    return false;
  }

  for (int i = 0; i < count; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) {
      continue;
    }

    bool merged = false;
    for (auto& existing : networks) {
      if (existing.ssid == ssid) {
        if (WiFi.RSSI(i) > existing.rssi) {
          existing.rssi = WiFi.RSSI(i);
          existing.secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        }
        merged = true;
        break;
      }
    }

    if (!merged) {
      NetworkInfo info;
      info.ssid = ssid;
      info.rssi = WiFi.RSSI(i);
      info.secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      networks.push_back(info);
    }
  }

  std::sort(networks.begin(), networks.end(),
            [](const NetworkInfo& a, const NetworkInfo& b) { return a.rssi > b.rssi; });

  if (networks.size() > AppConfig::kWifiScanMaxResults) {
    networks.resize(AppConfig::kWifiScanMaxResults);
  }
  return true;
}

int WifiProvisioner::pickNetwork(const std::vector<NetworkInfo>& networks) {
  const uint16_t headerBg = tft_.color565(15, 15, 25);
  const uint16_t specialBg = tft_.color565(40, 70, 140);
  const uint16_t rowDiv = tft_.color565(40, 40, 40);

  auto drawSignalBars = [&](int x, int y, int rssi) {
    const int bars = (rssi > -60) ? 4 : (rssi > -70) ? 3 : (rssi > -80) ? 2 : 1;
    const uint16_t activeColor =
        (rssi > -60) ? TFT_GREEN : (rssi > -70) ? tft_.color565(150, 220, 0)
                                                 : (rssi > -80) ? TFT_YELLOW : TFT_RED;
    static const int barH[4] = {5, 8, 12, 16};
    const int bottom = y + 18;
    for (int b = 0; b < 4; b++) {
      tft_.fillRect(x + b * 6, bottom - barH[b], 4, barH[b],
                    (b < bars) ? activeColor : rowDiv);
    }
  };

  tft_.fillScreen(TFT_BLACK);
  tft_.setTextDatum(ML_DATUM);
  tft_.fillRect(0, 0, AppConfig::kScreenWidth, kHeaderH, headerBg);
  tft_.setTextColor(TFT_CYAN, headerBg);
  tft_.drawString("Select WiFi Network", 10, kHeaderH / 2, 2);

  tft_.fillRoundRect(248, 4, 64, 20, 4, specialBg);
  tft_.setTextDatum(MC_DATUM);
  tft_.setTextColor(TFT_WHITE, specialBg);
  tft_.drawString("Skip", 280, 14, 1);

  if (networks.empty()) {
    tft_.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft_.drawString("No networks found.", AppConfig::kScreenWidth / 2,
                    AppConfig::kScreenHeight / 2, 2);
    tft_.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft_.drawString("Tap to rescan", AppConfig::kScreenWidth / 2,
                    AppConfig::kScreenHeight / 2 + 20, 1);
  }

  for (size_t i = 0; i < networks.size(); ++i) {
    const int y = kHeaderH + static_cast<int>(i) * kRowH;
    if (y + kRowH > AppConfig::kScreenHeight) {
      break;
    }

    tft_.fillRect(0, y, AppConfig::kScreenWidth, kRowH, TFT_BLACK);
    drawSignalBars(8, y + 2, networks[i].rssi);

    String ssid = networks[i].ssid;
    if (ssid.length() > 22) {
      ssid = ssid.substring(0, 20) + "..";
    }
    tft_.setTextDatum(ML_DATUM);
    tft_.setTextColor(tft_.color565(210, 210, 210), TFT_BLACK);
    tft_.drawString(ssid, 40, y + kRowH / 2, 2);

    tft_.setTextDatum(MR_DATUM);
    tft_.setTextColor(tft_.color565(120, 120, 120), TFT_BLACK);
    tft_.drawString(String(networks[i].rssi) + "dBm", networks[i].secure ? 282 : 312,
                    y + kRowH / 2, 1);

    if (networks[i].secure) {
      const uint16_t wpaBg = tft_.color565(120, 90, 0);
      tft_.fillRoundRect(291, y + 5, 22, 13, 3, wpaBg);
      tft_.setTextDatum(MC_DATUM);
      tft_.setTextColor(TFT_YELLOW, wpaBg);
      tft_.drawString("WPA", 302, y + kRowH / 2, 1);
    }

    tft_.drawFastHLine(0, y + kRowH - 1, AppConfig::kScreenWidth, rowDiv);
  }

  while (true) {
    uint16_t x = 0;
    uint16_t y = 0;
    if (!readTouch(x, y)) {
      delay(20);
      continue;
    }

    if (y < kHeaderH && x >= 248) {
      waitForTouchRelease();
      return -2;
    }

    if (y < kHeaderH) {
      waitForTouchRelease();
      return -1;
    }

    const int row = (y - kHeaderH) / kRowH;
    waitForTouchRelease();
    if (row >= 0 && row < static_cast<int>(networks.size())) {
      return row;
    }
  }
}

String WifiProvisioner::promptPassword(const String& ssid) {
  bool symMode = false;
  bool capsOn = true;
  String input;

  while (true) {
    const uint16_t keyBg = tft_.color565(35, 35, 50);
    const uint16_t keySpecial = tft_.color565(40, 70, 140);
    const uint16_t keyGreen = tft_.color565(20, 140, 60);
    const uint16_t keyActive = tft_.color565(60, 120, 210);

    tft_.fillScreen(TFT_BLACK);
    tft_.setTextDatum(ML_DATUM);
    tft_.setTextColor(tft_.color565(120, 200, 255), TFT_BLACK);
    String ssidLabel = "Network: ";
    ssidLabel += (ssid.length() > 22) ? ssid.substring(0, 20) + ".." : ssid;
    tft_.drawString(ssidLabel, 8, 10, 1);

    tft_.fillRoundRect(272, 2, 42, 18, 3, keySpecial);
    tft_.setTextDatum(MC_DATUM);
    tft_.setTextColor(TFT_WHITE, keySpecial);
    tft_.drawString("< Back", 293, 11, 1);

    const uint16_t fieldBg = tft_.color565(18, 18, 28);
    tft_.fillRoundRect(4, 22, 312, 26, 4, fieldBg);
    tft_.drawRoundRect(4, 22, 312, 26, 4, tft_.color565(70, 70, 100));

    String display;
    display.reserve(input.length() + 2);
    for (size_t i = 0; i < input.length(); ++i) {
      display += '*';
    }
    display += "_";
    if (display.length() > 38) {
      display = display.substring(display.length() - 38);
    }
    tft_.setTextDatum(ML_DATUM);
    tft_.setTextColor(TFT_WHITE, fieldBg);
    tft_.drawString(display, 10, 35, 2);

    tft_.setTextDatum(MR_DATUM);
    tft_.setTextColor(tft_.color565(130, 130, 130), fieldBg);
    tft_.drawString(String(input.length()), 308, 35, 1);

    std::vector<KeyRect> keys;

    auto addCharRow = [&](const char* chars, int y, int startX, int count) {
      for (int i = 0; i < count; ++i) {
        KeyRect k;
        k.x = startX + i * kKS;
        k.y = y;
        k.w = kKW;
        k.h = kKH;
        k.ch = chars[i];
        if (!symMode && !capsOn) {
          k.label = String((char)tolower(chars[i]));
        } else {
          k.label = String(chars[i]);
        }
        k.action = kChar;
        keys.push_back(k);
      }
    };

    if (symMode) {
      addCharRow("1234567890", kKY0, kRX10, 10);
      addCharRow("!@#$%^&*()", kKY1, kRX10, 10);
      addCharRow("-_=+.,/?;'", kKY2, kAlpha7X, 7);
    } else {
      addCharRow("QWERTYUIOP", kKY0, kRX10, 10);
      addCharRow("ASDFGHJKL", kKY1, kRX9, 9);
      addCharRow("ZXCVBNM", kKY2, kAlpha7X, 7);
    }

    auto addAction = [&](int x, int y, int w, const char* label, KeyAction action) {
      KeyRect k;
      k.x = x;
      k.y = y;
      k.w = w;
      k.h = kKH;
      k.label = label;
      k.action = action;
      keys.push_back(k);
    };

    addAction(kShiftX, kKY2, kShiftW, symMode ? "---" : (capsOn ? "CAP" : "shf"),
              kToggleMode);
    addAction(kBsX, kKY2, kBsW, "< X", kBackspace);
    addAction(kModeX, kKY3, kModeW, symMode ? "ABC" : "123", kToggleMode);
    addAction(kSpaceX, kKY3, kSpaceW, "SPACE", kSpace);
    addAction(kEnterX, kKY3, kEnterW, "OK", kDone);

    for (const auto& k : keys) {
      uint16_t bg = keyBg;
      if (k.action == kDone) {
        bg = keyGreen;
      } else if (k.action != kChar) {
        bg = keySpecial;
      }
      if (k.label == "CAP") {
        bg = keyActive;
      }

      tft_.fillRoundRect(k.x, k.y, k.w, k.h, 3, bg);
      tft_.setTextDatum(MC_DATUM);
      tft_.setTextColor(TFT_WHITE, bg);
      tft_.drawString(k.label, k.x + k.w / 2, k.y + k.h / 2, 2);
    }

    uint16_t tx = 0;
    uint16_t ty = 0;
    while (!readTouch(tx, ty)) {
      delay(20);
    }

    if (ty < 22 && tx >= 272) {
      waitForTouchRelease();
      return "__CANCEL__";
    }

    for (const auto& k : keys) {
      if (tx < k.x || tx >= k.x + k.w || ty < k.y || ty >= k.y + k.h) {
        continue;
      }

      waitForTouchRelease();
      if (k.action == kChar) {
        if (input.length() < 63) {
          char c = k.ch;
          if (!symMode && !capsOn) {
            c = (char)tolower(c);
          }
          input += c;
          if (!symMode && capsOn && input.length() == 1) {
            capsOn = false;
          }
        }
      } else if (k.action == kBackspace) {
        if (input.length() > 0) {
          input.remove(input.length() - 1);
        }
      } else if (k.action == kSpace) {
        if (input.length() < 63) {
          input += ' ';
        }
      } else if (k.action == kDone) {
        return input;
      } else if (k.action == kToggleMode) {
        if (k.label == "CAP" || k.label == "shf") {
          capsOn = !capsOn;
        } else {
          symMode = !symMode;
          if (!symMode) {
            capsOn = true;
          }
        }
      }
      break;
    }
  }
}

void WifiProvisioner::drawStatus(const String& line1, const String& line2) {
  tft_.fillScreen(TFT_BLACK);
  tft_.setTextDatum(MC_DATUM);
  tft_.setTextColor(TFT_CYAN, TFT_BLACK);
  tft_.drawString(line1, AppConfig::kScreenWidth / 2, AppConfig::kScreenHeight / 2 - 14, 2);

  if (line2.length() > 0) {
    tft_.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft_.drawString(line2, AppConfig::kScreenWidth / 2, AppConfig::kScreenHeight / 2 + 10,
                    2);
  }
  tft_.setTextDatum(TL_DATUM);
}

bool WifiProvisioner::readTouch(uint16_t& x, uint16_t& y) {
  if (!AppConfig::kTouchEnabled) {
    return false;
  }
  if (!touch_.touched()) {
    return false;
  }

  TS_Point raw = touch_.getPoint();
  TouchPoint mapped;
  if (!TouchMapper::mapRaw(raw, mapped)) {
    return false;
  }

  x = mapped.x;
  y = mapped.y;
  delay(AppConfig::kTouchDebounceMs);
  return true;
}

void WifiProvisioner::waitForTouchRelease() {
  if (!AppConfig::kTouchEnabled) {
    return;
  }
  while (touch_.touched()) {
    delay(15);
  }
}

void WifiProvisioner::persistCredentials(const String& ssid, const String& password) {
  prefs_.begin(kPrefsNamespace, false);
  prefs_.putString(kPrefsSsidKey, ssid);
  prefs_.putString(kPrefsPassKey, password);
  prefs_.end();
}
