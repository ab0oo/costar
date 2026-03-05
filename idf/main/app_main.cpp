#include "platform/Platform.h"
#include "RuntimeSettings.h"
#include "ConfigScreenEspIdf.h"
#include "DisplayBootstrapEspIdf.h"
#include "DisplaySpiEspIdf.h"
#include "GeoLocation.h"
#include "LayoutRuntimeEspIdf.h"
#include "TouchCalibration.h"
#include "TouchInputEspIdf.h"
#include "TextEntryEspIdf.h"
#include "LvglPasswordPrompt.h"
#include "Font5x7Classic.h"
#include "core/BootCommon.h"
#include "core/TimeSync.h"
#include "platform/Fs.h"
#include "platform/Net.h"
#include "platform/Prefs.h"
#include "AppConfig.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <vector>

namespace {
constexpr const char* kTag = "costar-idf";
constexpr const char* kBootTag = "boot";
constexpr const char* kWifiTag = "wifi";
constexpr const char* kFsTag = "fs";
constexpr const char* kTouchTag = "touch";
constexpr const char* kUiTag = "ui";
constexpr uint32_t kTouchBootProbeMs = 0;
constexpr uint32_t kConfigPostFailMs = 12000;
constexpr uint32_t kConfigPostConnectMs = 2500;
constexpr bool kBaselineEnabled = true;
constexpr unsigned long kBaselineLoopPeriodMs = 30000UL;
constexpr unsigned long kRuntimeTickPeriodMs = 33UL;
constexpr uint32_t kBootTaskStackBytes = 16384;
constexpr const char* kLayoutPrefsNs = "ui";
constexpr const char* kLayoutPrefsKey = "layout";
constexpr const char* kLayoutAPath = "/littlefs/screen_layout_a.json";
constexpr const char* kLayoutBPath = "/littlefs/screen_layout_b.json";
constexpr const char* kLayoutNytPath = "/littlefs/screen_layout_nyt.json";
constexpr const char* kLayoutQuakesPath = "/littlefs/screen_layout_quakes.json";
constexpr EventBits_t kWifiConnectedBit = BIT0;
constexpr EventBits_t kWifiFailedBit = BIT1;
EventGroupHandle_t sWifiEventGroup = nullptr;
bool sWifiStackReady = false;
bool sWifiHandlersRegistered = false;
TaskHandle_t sRuntimeTaskHandle = nullptr;

struct RuntimeLoopContext {
  boot::BaselineState baselineState;
  bool wifiReady = false;
  std::string activeLayoutPath = kLayoutAPath;
};

struct UiRect {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t w = 0;
  uint16_t h = 0;
};

struct RuntimeMenuRects {
  UiRect button;
  UiRect panel;
  UiRect rowLayoutA;
  UiRect rowLayoutB;
  UiRect rowLayoutNyt;
  UiRect rowLayoutQuakes;
  UiRect rowConfig;
  UiRect rowTouchCal;
};

enum class RuntimeMenuAction : uint8_t {
  None = 0,
  Toggle,
  SelectLayoutA,
  SelectLayoutB,
  SelectLayoutNyt,
  SelectLayoutQuakes,
  OpenConfig,
  OpenTouchCalibration,
  Dismiss,
};

struct RuntimeMenuState {
  bool open = false;
  bool dirty = true;
};

RuntimeMenuState sRuntimeMenu;

struct WifiApEntry {
  std::string ssid;
  int8_t rssi = -127;
  bool secure = true;
};


void logBuildFingerprint() {
  const esp_app_desc_t* desc = esp_app_get_description();
  if (desc == nullptr) {
    ESP_LOGW(kBootTag, "build id unavailable");
    return;
  }
  char shaHex[65] = {};
  for (size_t i = 0; i < 32; ++i) {
    std::snprintf(shaHex + (i * 2), 3, "%02x", static_cast<unsigned>(desc->app_elf_sha256[i]));
  }
  shaHex[64] = '\0';
  ESP_LOGI(kBootTag, "build id=%s", shaHex);
  ESP_LOGI(kBootTag, "build meta version=%s date=%s time=%s", desc->version, desc->date, desc->time);
}




void initNvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

void onWifiEvent(void* arg, esp_event_base_t eventBase, int32_t eventId, void* eventData) {
  (void)arg;
  (void)eventData;
  if (sWifiEventGroup == nullptr) {
    return;
  }

  if (eventBase == WIFI_EVENT) {
    if (eventId == WIFI_EVENT_STA_DISCONNECTED) {
      xEventGroupSetBits(sWifiEventGroup, kWifiFailedBit);
    }
  } else if (eventBase == IP_EVENT && eventId == IP_EVENT_STA_GOT_IP) {
    xEventGroupSetBits(sWifiEventGroup, kWifiConnectedBit);
  }
}

bool ensureWifiStackReady() {
  if (sWifiEventGroup == nullptr) {
    sWifiEventGroup = xEventGroupCreate();
    if (sWifiEventGroup == nullptr) {
      ESP_LOGE(kWifiTag, "event group alloc failed");
      return false;
    }
  }

  const esp_err_t loopErr = esp_event_loop_create_default();
  if (loopErr != ESP_OK && loopErr != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(kWifiTag, "event loop init failed err=0x%x", static_cast<unsigned>(loopErr));
    return false;
  }

  if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == nullptr) {
    esp_netif_create_default_wifi_sta();
  }

  static wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  const esp_err_t initErr = esp_wifi_init(&cfg);
  if (initErr != ESP_OK && initErr != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(kWifiTag, "wifi init failed err=0x%x", static_cast<unsigned>(initErr));
    return false;
  }

  if (!sWifiHandlersRegistered) {
    ESP_ERROR_CHECK(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &onWifiEvent, nullptr));
    ESP_ERROR_CHECK(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &onWifiEvent, nullptr));
    sWifiHandlersRegistered = true;
  }

  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  const esp_err_t startErr = esp_wifi_start();
  if (startErr != ESP_OK && startErr != ESP_ERR_WIFI_CONN) {
    ESP_LOGE(kWifiTag, "wifi start failed err=0x%x", static_cast<unsigned>(startErr));
    return false;
  }
  sWifiStackReady = true;
  return true;
}

void applySavedStaConfig() {
  // Reuse saved credentials when Wi-Fi driver NVS config is missing.
  const std::string savedSsid = platform::prefs::getString("wifi", "ssid", "");
  const std::string savedPass = platform::prefs::getString("wifi", "password", "");
  if (savedSsid.empty()) {
    return;
  }
  wifi_config_t staConfig = {};
  const size_t ssidLen =
      std::min(savedSsid.size(), sizeof(staConfig.sta.ssid) - static_cast<size_t>(1));
  std::memcpy(staConfig.sta.ssid, savedSsid.data(), ssidLen);

  const size_t passLen =
      std::min(savedPass.size(), sizeof(staConfig.sta.password) - static_cast<size_t>(1));
  std::memcpy(staConfig.sta.password, savedPass.data(), passLen);
  esp_err_t cfgErr = esp_wifi_set_config(WIFI_IF_STA, &staConfig);
  if (cfgErr == ESP_ERR_WIFI_STATE) {
    ESP_LOGW(kWifiTag, "set_config while busy; disconnecting and retrying");
    (void)esp_wifi_disconnect();
    platform::sleepMs(80);
    cfgErr = esp_wifi_set_config(WIFI_IF_STA, &staConfig);
  }
  if (cfgErr != ESP_OK) {
    ESP_LOGE(kWifiTag, "set_config failed err=0x%x", static_cast<unsigned>(cfgErr));
    return;
  }
  ESP_LOGI(kWifiTag, "loaded credentials from prefs ns=wifi");
}

bool startWifiStation(uint32_t timeoutMs, const char* requestedSsid = nullptr) {
  ESP_LOGI(kBootTag, "start wifi provisioning");
  if (!ensureWifiStackReady()) {
    return false;
  }
  if (requestedSsid != nullptr && *requestedSsid != '\0') {
    config_screen::showWifiStatus("CONNECTING WIFI", requestedSsid, false);
  } else {
    config_screen::showWifiStatus("CONNECTING WIFI", "TRYING SAVED CREDENTIALS", false);
  }
  (void)esp_wifi_disconnect();
  applySavedStaConfig();
  ESP_LOGI(kWifiTag, "station mode enabled");
  xEventGroupClearBits(sWifiEventGroup, kWifiConnectedBit | kWifiFailedBit);
  ESP_ERROR_CHECK(esp_wifi_connect());

  const EventBits_t bits =
      xEventGroupWaitBits(sWifiEventGroup, kWifiConnectedBit | kWifiFailedBit, pdFALSE, pdFALSE,
                          pdMS_TO_TICKS(timeoutMs));
  if ((bits & kWifiConnectedBit) != 0) {
    ESP_LOGI(kWifiTag, "connected with stored credentials");
    config_screen::showWifiStatus("WIFI CONNECTED", requestedSsid != nullptr ? requestedSsid : "", false);
    platform::sleepMs(500);
    return true;
  }
  ESP_LOGW(kWifiTag, "connect timeout/no stored credentials");
  config_screen::showWifiStatus("CONNECT FAILED", "TAP RETRY OR SCAN", true);
  platform::sleepMs(700);
  return false;
}

bool scanWifiNetworks(std::vector<WifiApEntry>& out) {
  out.clear();
  if (!ensureWifiStackReady()) {
    return false;
  }
  wifi_scan_config_t cfg = {};
  cfg.show_hidden = false;
  cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  if (esp_wifi_scan_start(&cfg, true) != ESP_OK) {
    return false;
  }

  uint16_t count = 0;
  if (esp_wifi_scan_get_ap_num(&count) != ESP_OK || count == 0) {
    return true;
  }
  std::vector<wifi_ap_record_t> records(count);
  if (esp_wifi_scan_get_ap_records(&count, records.data()) != ESP_OK) {
    return false;
  }

  for (uint16_t i = 0; i < count; ++i) {
    const wifi_ap_record_t& ap = records[i];
    if (ap.ssid[0] == '\0') {
      continue;
    }
    const std::string ssid(reinterpret_cast<const char*>(ap.ssid));
    auto it = std::find_if(out.begin(), out.end(), [&](const WifiApEntry& e) { return e.ssid == ssid; });
    if (it == out.end()) {
      WifiApEntry entry = {};
      entry.ssid = ssid;
      entry.rssi = ap.rssi;
      entry.secure = ap.authmode != WIFI_AUTH_OPEN;
      out.push_back(entry);
    } else if (ap.rssi > it->rssi) {
      it->rssi = ap.rssi;
      it->secure = ap.authmode != WIFI_AUTH_OPEN;
    }
  }

  std::sort(out.begin(), out.end(),
            [](const WifiApEntry& a, const WifiApEntry& b) { return a.rssi > b.rssi; });
  if (out.size() > AppConfig::kWifiScanMaxResults) {
    out.resize(AppConfig::kWifiScanMaxResults);
  }
  return true;
}

bool hasStoredWifiCreds() {
  const std::string savedSsid = platform::prefs::getString("wifi", "ssid", "");
  return !savedSsid.empty();
}

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

bool rectContains(const UiRect& r, uint16_t x, uint16_t y) {
  const uint16_t x2 = static_cast<uint16_t>(r.x + r.w);
  const uint16_t y2 = static_cast<uint16_t>(r.y + r.h);
  return x >= r.x && x < x2 && y >= r.y && y < y2;
}

void drawTinyChar(int x, int y, char c, uint16_t fg, uint16_t bg, int scale) {
  if (c < 0x20 || c > 0x7E) {
    c = '?';
  }
  const size_t idx = static_cast<size_t>(static_cast<uint8_t>(c)) * 5U;
  for (int col = 0; col < 5; ++col) {
    const uint8_t line = font[idx + static_cast<size_t>(col)];
    for (int row = 0; row < 8; ++row) {
      const bool on = ((line >> row) & 0x01U) != 0U;
      (void)display_spi::fillRect(static_cast<uint16_t>(x + col * scale),
                                  static_cast<uint16_t>(y + row * scale),
                                  static_cast<uint16_t>(scale),
                                  static_cast<uint16_t>(scale),
                                  on ? fg : bg);
    }
  }
}

void drawTinyText(int x, int y, const char* text, uint16_t fg, uint16_t bg, int scale) {
  if (text == nullptr) {
    return;
  }
  int penX = x;
  for (const char* p = text; *p != '\0'; ++p) {
    if (*p == ' ') {
      penX += scale * 6;
      continue;
    }
    drawTinyChar(penX, y, *p, fg, bg, scale);
    penX += scale * 6;
  }
}

RuntimeMenuRects calcRuntimeMenuRects() {
  RuntimeMenuRects out = {};
  const uint16_t w = display_spi::width();
  const uint16_t h = display_spi::height();
  const uint16_t menuBtnW = 24;
  const uint16_t menuBtnH = 20;
  const uint16_t margin = 4;
  (void)w;
  out.button = {margin, static_cast<uint16_t>(h - menuBtnH - margin), menuBtnW, menuBtnH};

  const uint16_t panelW = 160;
  const uint16_t rowH = 18;
  const uint16_t panelH = static_cast<uint16_t>(rowH * 6 + 6);
  int panelY = static_cast<int>(out.button.y) - static_cast<int>(panelH) - 4;
  if (panelY < 0) {
    panelY = 0;
  }
  out.panel = {margin, static_cast<uint16_t>(panelY), panelW, panelH};
  out.rowLayoutA = {static_cast<uint16_t>(out.panel.x + 3), static_cast<uint16_t>(out.panel.y + 3),
                    static_cast<uint16_t>(panelW - 6), rowH};
  out.rowLayoutB = {static_cast<uint16_t>(out.panel.x + 3), static_cast<uint16_t>(out.panel.y + 3 + rowH),
                    static_cast<uint16_t>(panelW - 6), rowH};
  out.rowLayoutNyt = {static_cast<uint16_t>(out.panel.x + 3), static_cast<uint16_t>(out.panel.y + 3 + rowH * 2),
                      static_cast<uint16_t>(panelW - 6), rowH};
  out.rowLayoutQuakes = {static_cast<uint16_t>(out.panel.x + 3), static_cast<uint16_t>(out.panel.y + 3 + rowH * 3),
                         static_cast<uint16_t>(panelW - 6), rowH};
  out.rowConfig = {static_cast<uint16_t>(out.panel.x + 3), static_cast<uint16_t>(out.panel.y + 3 + rowH * 4),
                   static_cast<uint16_t>(panelW - 6), rowH};
  out.rowTouchCal = {static_cast<uint16_t>(out.panel.x + 3), static_cast<uint16_t>(out.panel.y + 3 + rowH * 5),
                     static_cast<uint16_t>(panelW - 6), rowH};
  return out;
}

void drawRuntimeMenuButton(bool active) {
  const RuntimeMenuRects r = calcRuntimeMenuRects();
  const uint16_t bg = active ? rgb565(70, 90, 130) : rgb565(22, 31, 46);
  const uint16_t line = rgb565(220, 234, 248);
  (void)display_spi::fillRect(r.button.x, r.button.y, r.button.w, r.button.h, bg);
  (void)display_spi::fillRect(r.button.x, r.button.y, r.button.w, 1, line);
  (void)display_spi::fillRect(r.button.x, static_cast<uint16_t>(r.button.y + r.button.h - 1), r.button.w, 1, line);
  (void)display_spi::fillRect(r.button.x, r.button.y, 1, r.button.h, line);
  (void)display_spi::fillRect(static_cast<uint16_t>(r.button.x + r.button.w - 1), r.button.y, 1, r.button.h, line);
  const uint16_t barW = static_cast<uint16_t>(r.button.w - 10);
  for (int i = 0; i < 3; ++i) {
    const uint16_t y = static_cast<uint16_t>(r.button.y + 5 + i * 5);
    (void)display_spi::fillRect(static_cast<uint16_t>(r.button.x + 5), y, barW, 2, line);
  }
}

void drawRuntimeMenuOverlay(const std::string& activeLayoutPath) {
  const RuntimeMenuRects r = calcRuntimeMenuRects();
  const uint16_t panelBg = rgb565(10, 16, 28);
  const uint16_t border = rgb565(160, 185, 214);
  const uint16_t rowBg = rgb565(22, 34, 54);
  const uint16_t rowActive = rgb565(58, 92, 122);
  const uint16_t rowText = rgb565(225, 235, 245);

  (void)display_spi::fillRect(r.panel.x, r.panel.y, r.panel.w, r.panel.h, panelBg);
  (void)display_spi::fillRect(r.panel.x, r.panel.y, r.panel.w, 1, border);
  (void)display_spi::fillRect(r.panel.x, static_cast<uint16_t>(r.panel.y + r.panel.h - 1), r.panel.w, 1, border);
  (void)display_spi::fillRect(r.panel.x, r.panel.y, 1, r.panel.h, border);
  (void)display_spi::fillRect(static_cast<uint16_t>(r.panel.x + r.panel.w - 1), r.panel.y, 1, r.panel.h, border);

  const bool layoutAActive = activeLayoutPath == kLayoutAPath;
  const bool layoutBActive = activeLayoutPath == kLayoutBPath;
  const bool layoutNytActive = activeLayoutPath == kLayoutNytPath;
  const bool layoutQuakesActive = activeLayoutPath == kLayoutQuakesPath;
  (void)display_spi::fillRect(r.rowLayoutA.x, r.rowLayoutA.y, r.rowLayoutA.w, r.rowLayoutA.h,
                              layoutAActive ? rowActive : rowBg);
  (void)display_spi::fillRect(r.rowLayoutB.x, r.rowLayoutB.y, r.rowLayoutB.w, r.rowLayoutB.h,
                              layoutBActive ? rowActive : rowBg);
  (void)display_spi::fillRect(r.rowLayoutNyt.x, r.rowLayoutNyt.y, r.rowLayoutNyt.w, r.rowLayoutNyt.h,
                              layoutNytActive ? rowActive : rowBg);
  (void)display_spi::fillRect(r.rowLayoutQuakes.x, r.rowLayoutQuakes.y, r.rowLayoutQuakes.w, r.rowLayoutQuakes.h,
                              layoutQuakesActive ? rowActive : rowBg);
  (void)display_spi::fillRect(r.rowConfig.x, r.rowConfig.y, r.rowConfig.w, r.rowConfig.h, rowBg);
  (void)display_spi::fillRect(r.rowTouchCal.x, r.rowTouchCal.y, r.rowTouchCal.w, r.rowTouchCal.h, rowBg);

  drawTinyText(r.rowLayoutA.x + 4, r.rowLayoutA.y + 5, "Layout A (HA)", rowText,
               layoutAActive ? rowActive : rowBg, 1);
  drawTinyText(r.rowLayoutB.x + 4, r.rowLayoutB.y + 5, "Layout B (WX)", rowText,
               layoutBActive ? rowActive : rowBg, 1);
  drawTinyText(r.rowLayoutNyt.x + 4, r.rowLayoutNyt.y + 5, "Layout C (NYT)", rowText,
               layoutNytActive ? rowActive : rowBg, 1);
  drawTinyText(r.rowLayoutQuakes.x + 4, r.rowLayoutQuakes.y + 5, "Layout D (QK)", rowText,
               layoutQuakesActive ? rowActive : rowBg, 1);
  drawTinyText(r.rowConfig.x + 4, r.rowConfig.y + 5, "WiFi / Units", rowText, rowBg, 1);
  drawTinyText(r.rowTouchCal.x + 4, r.rowTouchCal.y + 5, "Touch Calibrate", rowText, rowBg, 1);
}

RuntimeMenuAction hitTestRuntimeMenu(uint16_t x, uint16_t y, bool menuOpen) {
  const RuntimeMenuRects r = calcRuntimeMenuRects();
  if (rectContains(r.button, x, y)) {
    return RuntimeMenuAction::Toggle;
  }
  if (!menuOpen) {
    return RuntimeMenuAction::None;
  }
  if (rectContains(r.rowLayoutA, x, y)) {
    return RuntimeMenuAction::SelectLayoutA;
  }
  if (rectContains(r.rowLayoutB, x, y)) {
    return RuntimeMenuAction::SelectLayoutB;
  }
  if (rectContains(r.rowLayoutNyt, x, y)) {
    return RuntimeMenuAction::SelectLayoutNyt;
  }
  if (rectContains(r.rowLayoutQuakes, x, y)) {
    return RuntimeMenuAction::SelectLayoutQuakes;
  }
  if (rectContains(r.rowConfig, x, y)) {
    return RuntimeMenuAction::OpenConfig;
  }
  if (rectContains(r.rowTouchCal, x, y)) {
    return RuntimeMenuAction::OpenTouchCalibration;
  }
  if (rectContains(r.panel, x, y)) {
    return RuntimeMenuAction::None;
  }
  return RuntimeMenuAction::Dismiss;
}

std::string loadPreferredLayoutPath() {
  std::string path = platform::prefs::getString(kLayoutPrefsNs, kLayoutPrefsKey, kLayoutAPath);
  if (path.empty()) {
    return kLayoutAPath;
  }
  if (path != kLayoutAPath && path != kLayoutBPath && path != kLayoutNytPath &&
      path != kLayoutQuakesPath) {
    return kLayoutAPath;
  }
  return path;
}

void savePreferredLayoutPath(const std::string& path) {
  if (path != kLayoutAPath && path != kLayoutBPath && path != kLayoutNytPath &&
      path != kLayoutQuakesPath) {
    return;
  }
  (void)platform::prefs::putString(kLayoutPrefsNs, kLayoutPrefsKey, path.c_str());
}






struct ConfigInteractionResult {
  bool offlineRequested = false;
  bool retryRequested = false;
  bool openWifiListRequested = false;
  bool localeChanged = false;
  std::string selectedSsid;
};

config_screen::ViewState makeViewState(bool hasStoredCreds, bool wifiConnected,
                                       bool showWifiButtons) {
  config_screen::ViewState state = {};
  state.hasStoredCreds = hasStoredCreds;
  state.wifiConnected = wifiConnected;
  state.showWifiButtons = showWifiButtons;
  state.use24HourClock = RuntimeSettings::use24HourClock;
  state.useFahrenheit = RuntimeSettings::useFahrenheit;
  state.useMiles = RuntimeSettings::useMiles;
  return state;
}

const char* configActionName(config_screen::Action action) {
  switch (action) {
    case config_screen::Action::RetryWifi:
      return "retry_wifi";
    case config_screen::Action::OfflineMode:
      return "offline_mode";
    case config_screen::Action::OpenWifiList:
      return "open_wifi_list";
    case config_screen::Action::ToggleClock:
      return "toggle_clock";
    case config_screen::Action::ToggleTemp:
      return "toggle_temp";
    case config_screen::Action::ToggleDistance:
      return "toggle_distance";
    default:
      return "none";
  }
}

ConfigInteractionResult runConfigInteraction(uint32_t durationMs, bool hasStoredCreds,
                                             bool wifiConnected, bool showWifiButtons) {
  ConfigInteractionResult result = {};
  config_screen::show(makeViewState(hasStoredCreds, wifiConnected, showWifiButtons));

  if (!AppConfig::kTouchEnabled) {
    return result;
  }
  if (!touch_input::init()) {
    ESP_LOGW(kTouchTag, "touch init failed");
    return result;
  }

  ESP_LOGI(kTouchTag, "interaction start duration_ms=%u wifi_buttons=%d",
           static_cast<unsigned>(durationMs), showWifiButtons ? 1 : 0);
  const uint32_t startMs = platform::millisMs();
  uint32_t lastLogMs = 0;
  bool touchHeld = false;
  while (durationMs == 0 || (platform::millisMs() - startMs < durationMs)) {
    touch_input::Point p;
    if (!touch_input::read(p)) {
      touchHeld = false;
      platform::sleepMs(15);
      continue;
    }
    config_screen::markTouch(p.x, p.y);

    if (touchHeld) {
      platform::sleepMs(25);
      continue;
    }
    touchHeld = true;

    const config_screen::Action action = config_screen::hitTest(p.x, p.y);
    const uint32_t now = platform::millisMs();
    if (action != config_screen::Action::None || now - lastLogMs > 250U) {
      ESP_LOGI(kTouchTag, "tap raw=(%u,%u) map=(%u,%u) action=%s", p.rawX, p.rawY, p.x, p.y,
               configActionName(action));
      lastLogMs = now;
    }

    if (action == config_screen::Action::ToggleClock) {
      RuntimeSettings::use24HourClock = !RuntimeSettings::use24HourClock;
      RuntimeSettings::save();
      result.localeChanged = true;
      config_screen::show(makeViewState(hasStoredCreds, wifiConnected, showWifiButtons));
    } else if (action == config_screen::Action::ToggleTemp) {
      RuntimeSettings::useFahrenheit = !RuntimeSettings::useFahrenheit;
      RuntimeSettings::save();
      result.localeChanged = true;
      config_screen::show(makeViewState(hasStoredCreds, wifiConnected, showWifiButtons));
    } else if (action == config_screen::Action::ToggleDistance) {
      RuntimeSettings::useMiles = !RuntimeSettings::useMiles;
      RuntimeSettings::save();
      result.localeChanged = true;
      config_screen::show(makeViewState(hasStoredCreds, wifiConnected, showWifiButtons));
    } else if (action == config_screen::Action::OpenWifiList && showWifiButtons) {
      result.openWifiListRequested = true;
      config_screen::showWifiScanInterstitial();
      std::vector<WifiApEntry> networks;
      if (!scanWifiNetworks(networks)) {
        ESP_LOGW(kWifiTag, "wifi scan failed");
        config_screen::show(makeViewState(hasStoredCreds, wifiConnected, showWifiButtons));
        platform::sleepMs(80);
        continue;
      }

      std::vector<std::string> labels;
      labels.reserve(networks.size());
      for (const auto& ap : networks) {
        std::string label = ap.ssid;
        label += ap.secure ? " WPA " : " OPEN ";
        label += std::to_string(static_cast<int>(ap.rssi));
        label += "DBM";
        labels.push_back(label);
      }
      std::vector<const char*> ptrs;
      ptrs.reserve(labels.size());
      for (const auto& label : labels) {
        ptrs.push_back(label.c_str());
      }

      // Drain the touch that triggered SCAN so the AP list doesn't consume a stale press.
      {
        const uint32_t releaseStart = platform::millisMs();
        touch_input::Point discard;
        while (platform::millisMs() - releaseStart < 800U) {
          if (!touch_input::read(discard)) {
            break;
          }
          platform::sleepMs(15);
        }
      }

      config_screen::showWifiList(ptrs.empty() ? nullptr : ptrs.data(),
                                  static_cast<uint16_t>(ptrs.size()));
      ESP_LOGI(kWifiTag, "scan complete aps=%u", static_cast<unsigned>(networks.size()));

      bool inList = true;
      bool listTouchHeld = false;
      const uint32_t listStart = platform::millisMs();
      while (inList && (durationMs == 0 || platform::millisMs() - listStart < durationMs)) {
        touch_input::Point lp;
        if (!touch_input::read(lp)) {
          listTouchHeld = false;
          platform::sleepMs(15);
          continue;
        }
        config_screen::markTouch(lp.x, lp.y);
        if (listTouchHeld) {
          platform::sleepMs(25);
          continue;
        }
        listTouchHeld = true;

        const int row =
            config_screen::hitTestWifiListRow(lp.x, lp.y, static_cast<uint16_t>(networks.size()));
        if (row == -1) {
          inList = false;
          break;
        }
        if (row < 0 || row >= static_cast<int>(networks.size())) {
          platform::sleepMs(35);
          continue;
        }

        const WifiApEntry& selected = networks[static_cast<size_t>(row)];
        ESP_LOGI(kWifiTag, "selected ssid=%s secure=%d rssi=%d", selected.ssid.c_str(),
                 selected.secure ? 1 : 0, static_cast<int>(selected.rssi));

        std::string password;
        if (selected.secure) {
          if (!lvgl_password_prompt::prompt("WIFI PASSWORD", selected.ssid, password)) {
            text_entry::Options opts = {};
            opts.title = "WIFI PASSWORD";
            opts.subtitle = selected.ssid;
            opts.maskInput = true;
            opts.maxLen = 63;
            if (!text_entry::prompt(opts, password)) {
              config_screen::showWifiList(ptrs.empty() ? nullptr : ptrs.data(),
                                          static_cast<uint16_t>(ptrs.size()));
              platform::sleepMs(80);
              continue;
            }
          }
        }

        if (!platform::prefs::putString("wifi", "ssid", selected.ssid.c_str()) ||
            !platform::prefs::putString("wifi", "password", password.c_str())) {
          ESP_LOGW(kWifiTag, "failed to persist credentials");
        }
        result.selectedSsid = selected.ssid;
        result.retryRequested = true;
        return result;
      }

      config_screen::show(makeViewState(hasStoredCreds, wifiConnected, showWifiButtons));
    } else if (action == config_screen::Action::OfflineMode && showWifiButtons) {
      result.offlineRequested = true;
      break;
    } else if (action == config_screen::Action::RetryWifi && showWifiButtons) {
      result.retryRequested = true;
      break;
    }

    platform::sleepMs(35);
  }
  ESP_LOGI(kTouchTag, "interaction end");
  return result;
}

bool fileSizeBytes(const char* path, long& outSize) {
  outSize = 0;
  if (path == nullptr || *path == '\0') {
    return false;
  }
  std::FILE* fp = std::fopen(path, "rb");
  if (fp == nullptr) {
    return false;
  }
  if (std::fseek(fp, 0, SEEK_END) != 0) {
    std::fclose(fp);
    return false;
  }
  const long size = std::ftell(fp);
  std::fclose(fp);
  if (size < 0) {
    return false;
  }
  outSize = size;
  return true;
}

void verifyLittlefsAssets() {
  struct RequiredAsset {
    const char* name;
    const char* path;
  };

  static constexpr RequiredAsset kRequired[] = {
      {"layout_a", "/littlefs/screen_layout_a.json"},
      {"layout_b", "/littlefs/screen_layout_b.json"},
      {"layout_nyt", "/littlefs/screen_layout_nyt.json"},
      {"layout_quakes", "/littlefs/screen_layout_quakes.json"},
      {"dsl_weather_now", "/littlefs/dsl_available/weather_now.json"},
      {"dsl_forecast", "/littlefs/dsl_available/forecast.json"},
      {"dsl_clock_analog_full", "/littlefs/dsl_available/clock_analog_full.json"},
      {"dsl_ha_card", "/littlefs/dsl_available/homeassistant_control_card.json"},
  };

  int missing = 0;
  for (const auto& asset : kRequired) {
    long size = 0;
    if (!fileSizeBytes(asset.path, size)) {
      ++missing;
      ESP_LOGW(kFsTag, "missing asset=%s path=%s", asset.name, asset.path);
      continue;
    }
    ESP_LOGI(kFsTag, "asset=%s path=%s bytes=%ld", asset.name, asset.path, size);
  }

  if (missing == 0) {
    ESP_LOGI(kFsTag, "required assets OK");
  } else {
    ESP_LOGE(kFsTag, "required assets missing=%d", missing);
  }
}

void logHeapLargest() {
  const size_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const size_t largestDma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
  ESP_LOGI("perf", "heap_largest_8bit=%u heap_largest_dma=%u", static_cast<unsigned>(largest8),
           static_cast<unsigned>(largestDma));
}

GeoContext loadGeoContextFromPrefs() {
  constexpr const char* kGeoNs = "geo";
  constexpr const char* kModeKey = "mode";
  constexpr const char* kManualLatKey = "mlat";
  constexpr const char* kManualLonKey = "mlon";
  constexpr const char* kManualTzKey = "mtz";
  constexpr const char* kManualOffsetKey = "moff";
  constexpr const char* kCachedLatKey = "lat";
  constexpr const char* kCachedLonKey = "lon";
  constexpr const char* kCachedTzKey = "tz";
  constexpr const char* kCachedOffsetKey = "off_min";
  constexpr int kModeManual = 1;
  constexpr int kOffsetUnknown = -32768;

  GeoContext geo;

  const int mode = static_cast<int>(platform::prefs::getInt(kGeoNs, kModeKey, 0));
  const float manualLat = platform::prefs::getFloat(kGeoNs, kManualLatKey, NAN);
  const float manualLon = platform::prefs::getFloat(kGeoNs, kManualLonKey, NAN);
  const std::string manualTz = platform::prefs::getString(kGeoNs, kManualTzKey, "");
  const int manualOffset =
      static_cast<int>(platform::prefs::getInt(kGeoNs, kManualOffsetKey, kOffsetUnknown));
  if (mode == kModeManual && isGeoValid(manualLat, manualLon, manualTz)) {
    geo.lat = manualLat;
    geo.lon = manualLon;
    geo.timezone = manualTz;
    geo.hasUtcOffset = manualOffset != kOffsetUnknown;
    geo.utcOffsetMinutes = geo.hasUtcOffset ? manualOffset : 0;
    geo.source = "manual";
    geo.hasLocation = true;
    return geo;
  }

  const float cachedLat = platform::prefs::getFloat(kGeoNs, kCachedLatKey, NAN);
  const float cachedLon = platform::prefs::getFloat(kGeoNs, kCachedLonKey, NAN);
  const std::string cachedTz = platform::prefs::getString(kGeoNs, kCachedTzKey, "");
  const int cachedOffset =
      static_cast<int>(platform::prefs::getInt(kGeoNs, kCachedOffsetKey, kOffsetUnknown));
  if (isGeoValid(cachedLat, cachedLon, cachedTz)) {
    geo.lat = cachedLat;
    geo.lon = cachedLon;
    geo.timezone = cachedTz;
    geo.hasUtcOffset = cachedOffset != kOffsetUnknown;
    geo.utcOffsetMinutes = geo.hasUtcOffset ? cachedOffset : 0;
    geo.source = "nvs-cache";
    geo.hasLocation = true;
    return geo;
  }

  return geo;
}

constexpr const char* kGeoSsidNs = "geo_ssid";
constexpr int kGeoSsidSlots = 4;

std::string geoSsidSlotKey(int slot, const char* field) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "s%d_%s", slot, field);
  return std::string(buf);
}

bool loadGeoContextFromSsidCache(const std::string& ssid, GeoContext& outGeo, std::string* outLabel = nullptr,
                                 std::string* outCity = nullptr, std::string* outRegion = nullptr,
                                 std::string* outCountry = nullptr) {
  if (ssid.empty()) {
    return false;
  }
  constexpr int kOffsetUnknown = -32768;
  for (int slot = 0; slot < kGeoSsidSlots; ++slot) {
    const std::string slotSsid = platform::prefs::getString(
        kGeoSsidNs, geoSsidSlotKey(slot, "ssid").c_str(), "");
    if (slotSsid.empty() || slotSsid != ssid) {
      continue;
    }
    const float lat = platform::prefs::getFloat(kGeoSsidNs, geoSsidSlotKey(slot, "lat").c_str(), NAN);
    const float lon = platform::prefs::getFloat(kGeoSsidNs, geoSsidSlotKey(slot, "lon").c_str(), NAN);
    const std::string tz = platform::prefs::getString(
        kGeoSsidNs, geoSsidSlotKey(slot, "tz").c_str(), "");
    const int off = static_cast<int>(platform::prefs::getInt(
        kGeoSsidNs, geoSsidSlotKey(slot, "off").c_str(), kOffsetUnknown));
    if (!isGeoValid(lat, lon, tz)) {
      continue;
    }
    outGeo = {};
    outGeo.lat = lat;
    outGeo.lon = lon;
    outGeo.timezone = tz;
    outGeo.hasUtcOffset = off != kOffsetUnknown;
    outGeo.utcOffsetMinutes = outGeo.hasUtcOffset ? off : 0;
    outGeo.hasLocation = true;
    outGeo.source = "ssid-cache";
    if (outLabel != nullptr) {
      *outLabel = platform::prefs::getString(kGeoSsidNs, geoSsidSlotKey(slot, "lbl").c_str(), "");
    }
    if (outCity != nullptr) {
      *outCity = platform::prefs::getString(kGeoSsidNs, geoSsidSlotKey(slot, "city").c_str(), "");
    }
    if (outRegion != nullptr) {
      *outRegion = platform::prefs::getString(kGeoSsidNs, geoSsidSlotKey(slot, "reg").c_str(), "");
    }
    if (outCountry != nullptr) {
      *outCountry = platform::prefs::getString(kGeoSsidNs, geoSsidSlotKey(slot, "cty").c_str(), "");
    }
    return true;
  }
  return false;
}

void persistGeoContextToGlobalPrefs(const GeoContext& geo, const std::string& label, const std::string& city,
                                    const std::string& region, const std::string& country) {
  constexpr const char* kGeoNs = "geo";
  constexpr const char* kModeKey = "mode";
  constexpr const char* kCachedLatKey = "lat";
  constexpr const char* kCachedLonKey = "lon";
  constexpr const char* kCachedTzKey = "tz";
  constexpr const char* kCachedOffsetKey = "off_min";
  constexpr const char* kCachedLabelKey = "label";
  constexpr const char* kCachedCityKey = "city";
  constexpr const char* kCachedRegionKey = "region";
  constexpr const char* kCachedCountryKey = "country";
  constexpr int kModeAuto = 0;
  constexpr int kOffsetUnknown = -32768;
  (void)platform::prefs::putInt(kGeoNs, kModeKey, kModeAuto);
  (void)platform::prefs::putFloat(kGeoNs, kCachedLatKey, geo.lat);
  (void)platform::prefs::putFloat(kGeoNs, kCachedLonKey, geo.lon);
  (void)platform::prefs::putString(kGeoNs, kCachedTzKey, geo.timezone.c_str());
  (void)platform::prefs::putInt(kGeoNs, kCachedOffsetKey,
                                geo.hasUtcOffset ? geo.utcOffsetMinutes : kOffsetUnknown);
  (void)platform::prefs::putString(kGeoNs, kCachedLabelKey, label.c_str());
  (void)platform::prefs::putString(kGeoNs, kCachedCityKey, city.c_str());
  (void)platform::prefs::putString(kGeoNs, kCachedRegionKey, region.c_str());
  (void)platform::prefs::putString(kGeoNs, kCachedCountryKey, country.c_str());
}

void saveGeoContextToSsidCache(const std::string& ssid, const GeoContext& geo, const std::string& label,
                               const std::string& city, const std::string& region,
                               const std::string& country) {
  if (ssid.empty() || !geo.hasLocation) {
    return;
  }
  constexpr int kOffsetUnknown = -32768;
  int targetSlot = -1;
  int emptySlot = -1;
  int oldestSlot = 0;
  int oldestSeq = std::numeric_limits<int>::max();
  int maxSeq = 0;
  for (int slot = 0; slot < kGeoSsidSlots; ++slot) {
    const std::string ssidKey = geoSsidSlotKey(slot, "ssid");
    const std::string seqKey = geoSsidSlotKey(slot, "seq");
    const std::string slotSsid = platform::prefs::getString(kGeoSsidNs, ssidKey.c_str(), "");
    const int seq = static_cast<int>(platform::prefs::getInt(kGeoSsidNs, seqKey.c_str(), 0));
    maxSeq = std::max(maxSeq, seq);
    if (!slotSsid.empty() && slotSsid == ssid) {
      targetSlot = slot;
      break;
    }
    if (slotSsid.empty() && emptySlot < 0) {
      emptySlot = slot;
    }
    if (seq < oldestSeq) {
      oldestSeq = seq;
      oldestSlot = slot;
    }
  }
  if (targetSlot < 0) {
    targetSlot = (emptySlot >= 0) ? emptySlot : oldestSlot;
  }
  const int nextSeq = maxSeq + 1;
  (void)platform::prefs::putString(kGeoSsidNs, geoSsidSlotKey(targetSlot, "ssid").c_str(), ssid.c_str());
  (void)platform::prefs::putFloat(kGeoSsidNs, geoSsidSlotKey(targetSlot, "lat").c_str(), geo.lat);
  (void)platform::prefs::putFloat(kGeoSsidNs, geoSsidSlotKey(targetSlot, "lon").c_str(), geo.lon);
  (void)platform::prefs::putString(kGeoSsidNs, geoSsidSlotKey(targetSlot, "tz").c_str(), geo.timezone.c_str());
  (void)platform::prefs::putInt(kGeoSsidNs, geoSsidSlotKey(targetSlot, "off").c_str(),
                                geo.hasUtcOffset ? geo.utcOffsetMinutes : kOffsetUnknown);
  (void)platform::prefs::putString(kGeoSsidNs, geoSsidSlotKey(targetSlot, "lbl").c_str(), label.c_str());
  (void)platform::prefs::putString(kGeoSsidNs, geoSsidSlotKey(targetSlot, "city").c_str(), city.c_str());
  (void)platform::prefs::putString(kGeoSsidNs, geoSsidSlotKey(targetSlot, "reg").c_str(), region.c_str());
  (void)platform::prefs::putString(kGeoSsidNs, geoSsidSlotKey(targetSlot, "cty").c_str(), country.c_str());
  (void)platform::prefs::putInt(kGeoSsidNs, geoSsidSlotKey(targetSlot, "seq").c_str(), nextSeq);
}

GeoContext fallbackGeoContextGoogleHq() {
  constexpr const char* kGeoNs = "geo";
  constexpr const char* kModeKey = "mode";
  constexpr const char* kCachedLatKey = "lat";
  constexpr const char* kCachedLonKey = "lon";
  constexpr const char* kCachedTzKey = "tz";
  constexpr const char* kCachedOffsetKey = "off_min";
  constexpr const char* kCachedLabelKey = "label";
  constexpr int kModeAuto = 0;
  constexpr int kOffsetUnknown = -32768;

  GeoContext geo;
  geo.lat = 37.4220f;
  geo.lon = -122.0841f;
  geo.timezone = "America/Los_Angeles";
  geo.hasUtcOffset = false;
  geo.utcOffsetMinutes = 0;
  geo.source = "fallback-google-hq";
  geo.hasLocation = true;

  (void)platform::prefs::putInt(kGeoNs, kModeKey, kModeAuto);
  (void)platform::prefs::putFloat(kGeoNs, kCachedLatKey, geo.lat);
  (void)platform::prefs::putFloat(kGeoNs, kCachedLonKey, geo.lon);
  (void)platform::prefs::putString(kGeoNs, kCachedTzKey, geo.timezone.c_str());
  (void)platform::prefs::putInt(kGeoNs, kCachedOffsetKey, kOffsetUnknown);
  (void)platform::prefs::putString(kGeoNs, kCachedLabelKey, "Mountain View, CA, US");
  return geo;
}

void refreshLayout(RuntimeLoopContext* ctx) {
  if (ctx == nullptr) {
    return;
  }
  if (!layout_runtime::begin(ctx->activeLayoutPath.c_str())) {
    ESP_LOGW(kUiTag, "layout begin failed path=%s; falling back to layout A",
             ctx->activeLayoutPath.c_str());
    ctx->activeLayoutPath = kLayoutAPath;
    (void)layout_runtime::begin(kLayoutAPath);
  }
  sRuntimeMenu.dirty = true;
}

void switchLayout(RuntimeLoopContext* ctx, const char* path) {
  if (ctx == nullptr || path == nullptr || *path == '\0') {
    return;
  }
  ctx->activeLayoutPath = path;
  savePreferredLayoutPath(ctx->activeLayoutPath);
  ESP_LOGI(kUiTag, "switch layout path=%s", ctx->activeLayoutPath.c_str());
  refreshLayout(ctx);
}

void openRuntimeConfig(RuntimeLoopContext* ctx) {
  const bool hasCreds = hasStoredWifiCreds();
  const bool wifiConnected = platform::net::isConnected();
  ESP_LOGI(kUiTag, "open runtime config");
  const ConfigInteractionResult result =
      runConfigInteraction(20000, hasCreds, wifiConnected, true);
  if (result.retryRequested) {
    const char* requested =
        result.selectedSsid.empty() ? nullptr : result.selectedSsid.c_str();
    ctx->wifiReady = startWifiStation(10000, requested);
  } else {
    ctx->wifiReady = platform::net::isConnected();
  }
  refreshLayout(ctx);
}

void openRuntimeTouchCalibration(RuntimeLoopContext* ctx) {
  ESP_LOGI(kUiTag, "open runtime touch calibration");
  (void)runTouchCalibration(true);
  refreshLayout(ctx);
}

void runtimeLoopTask(void* arg) {
  RuntimeLoopContext* ctx = static_cast<RuntimeLoopContext*>(arg);
  if (ctx == nullptr) {
    vTaskDelete(nullptr);
    return;
  }
  ESP_LOGI(kTag, "runtime loop task started core=%d", static_cast<int>(xPortGetCoreID()));
  uint32_t lastTickMs = platform::millisMs();
  bool touchDown = false;
  uint16_t tapX = 0;
  uint16_t tapY = 0;
  uint32_t touchDownMs = 0;
  constexpr uint32_t kTapMaxMs = 700;
  for (;;) {
    platform::sleepMs(kRuntimeTickPeriodMs);
    const uint32_t nowMs = platform::millisMs();
    if (AppConfig::kTouchEnabled) {
      touch_input::Point p;
      if (touch_input::read(p)) {
        if (!touchDown) {
          touchDown = true;
          tapX = p.x;
          tapY = p.y;
          touchDownMs = nowMs;
          ESP_LOGI(kTouchTag, "runtime tap down x=%u y=%u", tapX, tapY);
        }
      } else if (touchDown) {
        const uint32_t heldMs = nowMs - touchDownMs;
        ESP_LOGI(kTouchTag, "runtime tap up x=%u y=%u held_ms=%u", tapX, tapY,
                 static_cast<unsigned>(heldMs));
        if (heldMs <= kTapMaxMs) {
          bool handled = false;
          const RuntimeMenuAction menuAction = hitTestRuntimeMenu(tapX, tapY, sRuntimeMenu.open);
          if (menuAction != RuntimeMenuAction::None) {
            handled = true;
            ESP_LOGI(kUiTag, "menu action=%d x=%u y=%u", static_cast<int>(menuAction), tapX, tapY);
            if (menuAction == RuntimeMenuAction::Toggle) {
              const bool newOpen = !sRuntimeMenu.open;
              if (newOpen != sRuntimeMenu.open) {
                sRuntimeMenu.open = newOpen;
                sRuntimeMenu.dirty = true;
                if (!sRuntimeMenu.open) {
                  refreshLayout(ctx);
                }
              }
            } else if (menuAction == RuntimeMenuAction::Dismiss) {
              if (sRuntimeMenu.open) {
                sRuntimeMenu.open = false;
                sRuntimeMenu.dirty = true;
                refreshLayout(ctx);
              }
            } else if (menuAction == RuntimeMenuAction::SelectLayoutA) {
              sRuntimeMenu.open = false;
              sRuntimeMenu.dirty = true;
              switchLayout(ctx, kLayoutAPath);
            } else if (menuAction == RuntimeMenuAction::SelectLayoutB) {
              sRuntimeMenu.open = false;
              sRuntimeMenu.dirty = true;
              switchLayout(ctx, kLayoutBPath);
            } else if (menuAction == RuntimeMenuAction::SelectLayoutNyt) {
              sRuntimeMenu.open = false;
              sRuntimeMenu.dirty = true;
              switchLayout(ctx, kLayoutNytPath);
            } else if (menuAction == RuntimeMenuAction::SelectLayoutQuakes) {
              sRuntimeMenu.open = false;
              sRuntimeMenu.dirty = true;
              switchLayout(ctx, kLayoutQuakesPath);
            } else if (menuAction == RuntimeMenuAction::OpenConfig) {
              sRuntimeMenu.open = false;
              sRuntimeMenu.dirty = true;
              openRuntimeConfig(ctx);
            } else if (menuAction == RuntimeMenuAction::OpenTouchCalibration) {
              sRuntimeMenu.open = false;
              sRuntimeMenu.dirty = true;
              openRuntimeTouchCalibration(ctx);
            }
          } else {
            handled = layout_runtime::onTap(tapX, tapY);
            if (handled) {
              // DSL tap handlers can redraw widgets; repaint the menu button
              // once so it stays visually on top.
              sRuntimeMenu.dirty = true;
            }
          }
          ESP_LOGI(kTouchTag, "runtime tap dispatch x=%u y=%u handled=%d menu_open=%d", tapX, tapY,
                   handled ? 1 : 0, sRuntimeMenu.open ? 1 : 0);
        }
        touchDown = false;
      }
    }
    if (!sRuntimeMenu.open) {
      if (layout_runtime::tick(nowMs)) {
        sRuntimeMenu.dirty = true;
      }
    }
    if (sRuntimeMenu.dirty) {
      drawRuntimeMenuButton(sRuntimeMenu.open);
      if (sRuntimeMenu.open) {
        drawRuntimeMenuOverlay(ctx->activeLayoutPath);
      }
      sRuntimeMenu.dirty = false;
    }
    if (nowMs - lastTickMs >= kBaselineLoopPeriodMs) {
      lastTickMs = nowMs;
      boot::markLoop(ctx->baselineState, ctx->wifiReady, kBaselineEnabled, kBaselineLoopPeriodMs);
      logHeapLargest();
    }
  }
}
void bootTask(void* arg) {
  (void)arg;
  boot::BaselineState baselineState;
  boot::start(baselineState);
  initNvs();
  ESP_ERROR_CHECK(esp_netif_init());

  ESP_LOGI(kTag, "WidgetOS boot (ESP-IDF scaffold)");
  logBuildFingerprint();
  ESP_LOGI(kBootTag, "setup start");
  boot::mark(baselineState, "setup_start", kBaselineEnabled);
  RuntimeSettings::load();
  boot::logSettingsSummary(true);
  const bool fsReady = platform::fs::begin(true);
  ESP_LOGI(kBootTag, "littlefs=%d", fsReady ? 1 : 0);
  if (fsReady) {
    verifyLittlefsAssets();
  }
  boot::mark(baselineState, "littlefs_ready", kBaselineEnabled);

  ESP_LOGI(kBootTag, "init backlight + TFT");
  display_bootstrap::initPins();
  if (!display_spi::init()) {
    ESP_LOGE(kBootTag, "TFT SPI init failed");
  } else if (!display_spi::initPanel()) {
    ESP_LOGE(kBootTag, "TFT panel init failed");
  } else if (!display_spi::clear(0x0000)) {
    ESP_LOGE(kBootTag, "TFT clear failed");
  }
  boot::mark(baselineState, "tft_ready", kBaselineEnabled);

  if (AppConfig::kTouchEnabled) {
    if (touch_input::init()) {
      ESP_LOGI(kTouchTag, "touch ready after tft init");
      (void)runTouchCalibration(false);
      (void)runDisplayModeCalibrationIfNeeded();
    } else {
      ESP_LOGE(kTouchTag, "touch init failed after tft init");
    }
  } else {
    (void)runDisplayModeCalibrationIfNeeded();
  }

  const bool savedCreds = hasStoredWifiCreds();
  bool wifiReady = false;
  bool offlineSelected = false;

  // Production-style behavior: if credentials exist, boot directly to app path.
  // Only enter config first-run flow when no credentials are stored.
  if (!savedCreds) {
    const ConfigInteractionResult preWifi =
        runConfigInteraction(kTouchBootProbeMs, savedCreds, false, true);
    if (preWifi.offlineRequested) {
      offlineSelected = true;
      ESP_LOGI(kWifiTag, "offline mode selected before connect");
    }
  } else {
    ESP_LOGI(kBootTag, "saved wifi creds present; skipping pre-wifi config");
  }

  if (!offlineSelected) {
    wifiReady = startWifiStation(10000, nullptr);
  } else {
    ESP_LOGI(kWifiTag, "skipping WiFi connect");
  }

  if (!wifiReady && !offlineSelected) {
    const ConfigInteractionResult postFail =
        runConfigInteraction(kConfigPostFailMs, savedCreds, false, true);
    if (postFail.offlineRequested) {
      offlineSelected = true;
      ESP_LOGI(kWifiTag, "offline mode selected after connect failure");
    } else if (postFail.retryRequested) {
      ESP_LOGI(kWifiTag, "retry requested from config screen");
      const char* requested =
          postFail.selectedSsid.empty() ? nullptr : postFail.selectedSsid.c_str();
      wifiReady = startWifiStation(10000, requested);
    }
  }

  boot::mark(baselineState, "wifi_ready", kBaselineEnabled);

  if (!wifiReady) {
    (void)runConfigInteraction(kConfigPostFailMs, savedCreds, false, true);
  } else if (!savedCreds) {
    (void)runConfigInteraction(kConfigPostConnectMs, true, true, false);
  }

  GeoContext geo = loadGeoContextFromPrefs();
  if (geo.hasLocation) {
    ESP_LOGI("geo", "source=%s lat=%.4f lon=%.4f tz=%s off_min=%d known=%d", geo.source.c_str(),
             geo.lat, geo.lon, geo.timezone.c_str(), geo.utcOffsetMinutes,
             geo.hasUtcOffset ? 1 : 0);
  } else {
    geo = fallbackGeoContextGoogleHq();
    ESP_LOGW("geo", "cache/override missing; using fallback source=%s lat=%.4f lon=%.4f tz=%s",
             geo.source.c_str(), geo.lat, geo.lon, geo.timezone.c_str());
  }

  std::string ipText;
  std::string connectedSsid;
  if (wifiReady && platform::net::getLocalIp(ipText) && !ipText.empty()) {
    ESP_LOGI(kWifiTag, "connected ip=%s", ipText.c_str());
  }
  if (wifiReady) {
    (void)platform::net::getSsid(connectedSsid);
  }

  if (wifiReady && geo.source != "manual") {
    bool usedSsidGeoCache = false;
    if (!connectedSsid.empty()) {
      GeoContext ssidCachedGeo;
      std::string cachedLabel;
      std::string cachedCity;
      std::string cachedRegion;
      std::string cachedCountry;
      if (loadGeoContextFromSsidCache(connectedSsid, ssidCachedGeo, &cachedLabel, &cachedCity, &cachedRegion,
                                      &cachedCountry)) {
        geo = ssidCachedGeo;
        persistGeoContextToGlobalPrefs(geo, cachedLabel, cachedCity, cachedRegion, cachedCountry);
        usedSsidGeoCache = true;
        ESP_LOGI("geo", "source=ssid-cache ssid=%s lat=%.4f lon=%.4f tz=%s off_min=%d known=%d",
                 connectedSsid.c_str(), geo.lat, geo.lon, geo.timezone.c_str(), geo.utcOffsetMinutes,
                 geo.hasUtcOffset ? 1 : 0);
      }
    }
    if (!usedSsidGeoCache) {
      GeoContext refreshedGeo;
      if (refreshGeoContextFromInternet(refreshedGeo)) {
        geo = refreshedGeo;
        const std::string label = platform::prefs::getString("geo", "label", "");
        const std::string city = platform::prefs::getString("geo", "city", "");
        const std::string region = platform::prefs::getString("geo", "region", "");
        const std::string country = platform::prefs::getString("geo", "country", "");
        if (!connectedSsid.empty()) {
          saveGeoContextToSsidCache(connectedSsid, geo, label, city, region, country);
        }
        ESP_LOGI("geo", "online source=%s lat=%.4f lon=%.4f tz=%s off_min=%d known=%d",
                 geo.source.c_str(), geo.lat, geo.lon, geo.timezone.c_str(), geo.utcOffsetMinutes,
                 geo.hasUtcOffset ? 1 : 0);
      } else {
        ESP_LOGW("geo", "online fetch failed; using cached timezone context");
      }
    } else {
      ESP_LOGI("geo", "ssid cache hit; skipped online geolocation fetch");
    }
  }

  // SNTP startup delay (CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY) can be up to
  // 5000ms, plus NTP round-trip. Use 15s to reliably cover both.
  (void)timesync::ensureUtcTime(15000);
  timesync::logUiTimeContext(geo.timezone.empty() ? nullptr : geo.timezone.c_str(),
                             geo.utcOffsetMinutes, geo.hasUtcOffset);
  boot::mark(baselineState, "geo_time_ready", kBaselineEnabled);

  std::string activeLayoutPath = loadPreferredLayoutPath();
  ESP_LOGI(kBootTag, "idf scaffold ready");
  boot::mark(baselineState, "display_ready", kBaselineEnabled);
  bool runtimeReady = layout_runtime::begin(activeLayoutPath.c_str());
  if (!runtimeReady && activeLayoutPath != kLayoutAPath) {
    ESP_LOGW(kUiTag, "preferred layout failed path=%s fallback=%s", activeLayoutPath.c_str(),
             kLayoutAPath);
    activeLayoutPath = kLayoutAPath;
    runtimeReady = layout_runtime::begin(activeLayoutPath.c_str());
  }
  ESP_LOGI(kBootTag, "layout runtime=%d", runtimeReady ? 1 : 0);
  ESP_LOGI(kBootTag, "setup complete");
  boot::mark(baselineState, "setup_complete", kBaselineEnabled);
  logHeapLargest();

  auto* loopCtx = new RuntimeLoopContext();
  if (loopCtx != nullptr) {
    loopCtx->baselineState = baselineState;
    loopCtx->wifiReady = wifiReady;
    loopCtx->activeLayoutPath = activeLayoutPath;
  }
  if (loopCtx == nullptr ||
      xTaskCreatePinnedToCore(runtimeLoopTask, "costar_runtime", 8192, loopCtx, 4, &sRuntimeTaskHandle,
                              1) != pdPASS) {
    ESP_LOGE(kTag, "failed to start runtime task on core 1; running inline");
    delete loopCtx;
    ESP_LOGI(kTag, "ESP-IDF runtime loop started (inline)");
    uint32_t lastTickMs = platform::millisMs();
    for (;;) {
      platform::sleepMs(kRuntimeTickPeriodMs);
      const uint32_t nowMs = platform::millisMs();
      (void)layout_runtime::tick(nowMs);
      if (nowMs - lastTickMs >= kBaselineLoopPeriodMs) {
        lastTickMs = nowMs;
        boot::markLoop(baselineState, wifiReady, kBaselineEnabled, kBaselineLoopPeriodMs);
        logHeapLargest();
      }
    }
  }

  ESP_LOGI(kTag, "runtime task pinned core=1; boot task exiting on core=%d",
           static_cast<int>(xPortGetCoreID()));
  vTaskDelete(nullptr);
}
}  // namespace

extern "C" void app_main() {
  if (xTaskCreatePinnedToCore(bootTask, "costar_boot", kBootTaskStackBytes, nullptr, 5, nullptr, 0) !=
      pdPASS) {
    ESP_LOGE(kTag, "failed to start boot task");
    for (;;) {
      platform::sleepMs(1000);
    }
  }
}
