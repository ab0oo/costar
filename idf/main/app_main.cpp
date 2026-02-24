#include "platform/Platform.h"
#include "RuntimeSettings.h"
#include "ConfigScreenEspIdf.h"
#include "DisplayBootstrapEspIdf.h"
#include "DisplaySpiEspIdf.h"
#include "TouchInputEspIdf.h"
#include "core/BootCommon.h"
#include "core/TimeSync.h"
#include "platform/Fs.h"
#include "platform/Net.h"
#include "platform/Prefs.h"
#include "AppConfig.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

namespace {
constexpr const char* kTag = "costar-idf";
constexpr const char* kBootTag = "boot";
constexpr const char* kWifiTag = "wifi";
constexpr const char* kFsTag = "fs";
constexpr const char* kTouchTag = "touch";
constexpr uint32_t kTouchBootProbeMs = 10000;
constexpr uint32_t kConfigPostFailMs = 12000;
constexpr uint32_t kConfigPostConnectMs = 2500;
constexpr bool kBaselineEnabled = true;
constexpr unsigned long kBaselineLoopPeriodMs = 30000UL;
constexpr EventBits_t kWifiConnectedBit = BIT0;
constexpr EventBits_t kWifiFailedBit = BIT1;

EventGroupHandle_t sWifiEventGroup = nullptr;
bool sWifiStackReady = false;
bool sWifiHandlersRegistered = false;

struct WifiApEntry {
  std::string ssid;
  int8_t rssi = -127;
  bool secure = true;
};

struct GeoContext {
  float lat = NAN;
  float lon = NAN;
  std::string timezone;
  int utcOffsetMinutes = 0;
  bool hasUtcOffset = false;
  std::string source = "none";
  bool hasLocation = false;
};

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

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
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
  // Reuse Arduino-side saved credentials when Wi-Fi driver NVS config is missing.
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

bool startWifiStation(uint32_t timeoutMs) {
  ESP_LOGI(kBootTag, "start wifi provisioning");
  if (!ensureWifiStackReady()) {
    return false;
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
    return true;
  }
  ESP_LOGW(kWifiTag, "connect timeout/no stored credentials");
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

struct ConfigInteractionResult {
  bool offlineRequested = false;
  bool retryRequested = false;
  bool openWifiListRequested = false;
  bool localeChanged = false;
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
  while (platform::millisMs() - startMs < durationMs) {
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

      config_screen::showWifiList(ptrs.empty() ? nullptr : ptrs.data(),
                                  static_cast<uint16_t>(ptrs.size()));
      ESP_LOGI(kWifiTag, "scan complete aps=%u", static_cast<unsigned>(networks.size()));

      bool inList = true;
      bool listTouchHeld = true;
      const uint32_t listStart = platform::millisMs();
      while (inList && platform::millisMs() - listStart < durationMs) {
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
        if (selected.secure) {
          ESP_LOGW(kWifiTag, "secure AP selected; password entry not implemented yet");
          inList = false;
          break;
        }

        if (!platform::prefs::putString("wifi", "ssid", selected.ssid.c_str()) ||
            !platform::prefs::putString("wifi", "password", "")) {
          ESP_LOGW(kWifiTag, "failed to persist open-network credentials");
        }
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
      {"layout_b", "/littlefs/screen_layout_b.json"},
      {"dsl_weather_now", "/littlefs/dsl_active/weather_now.json"},
      {"dsl_forecast", "/littlefs/dsl_active/forecast.json"},
      {"dsl_clock_analog_full", "/littlefs/dsl_active/clock_analog_full.json"},
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

bool isGeoValid(float lat, float lon, const std::string& timezone) {
  return !std::isnan(lat) && !std::isnan(lon) && !timezone.empty();
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
}  // namespace

extern "C" void app_main() {
  boot::BaselineState baselineState;
  boot::start(baselineState);
  initNvs();
  ESP_ERROR_CHECK(esp_netif_init());

  ESP_LOGI(kTag, "WidgetOS boot (ESP-IDF scaffold)");
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
  } else if (!display_spi::drawSanityPattern()) {
    ESP_LOGE(kBootTag, "TFT sanity pattern failed");
  }
  boot::mark(baselineState, "tft_ready", kBaselineEnabled);

  const bool savedCreds = hasStoredWifiCreds();
  bool wifiReady = false;
  bool offlineSelected = false;

  // Always present config UI briefly pre-WiFi to validate touch actions during porting.
  const ConfigInteractionResult preWifi =
      runConfigInteraction(kTouchBootProbeMs, savedCreds, false, true);
  if (preWifi.offlineRequested) {
    offlineSelected = true;
    ESP_LOGI(kWifiTag, "offline mode selected before connect");
  }

  if (!offlineSelected) {
    wifiReady = startWifiStation(10000);
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
      wifiReady = startWifiStation(10000);
    }
  }

  boot::mark(baselineState, "wifi_ready", kBaselineEnabled);

  if (!wifiReady) {
    (void)runConfigInteraction(kConfigPostFailMs, savedCreds, false, true);
  } else if (!savedCreds) {
    (void)runConfigInteraction(kConfigPostConnectMs, true, true, false);
  }

  const GeoContext geo = loadGeoContextFromPrefs();
  if (geo.hasLocation) {
    ESP_LOGI("geo", "source=%s lat=%.4f lon=%.4f tz=%s off_min=%d known=%d", geo.source.c_str(),
             geo.lat, geo.lon, geo.timezone.c_str(), geo.utcOffsetMinutes,
             geo.hasUtcOffset ? 1 : 0);
  } else {
    ESP_LOGW("geo", "cache/override missing; timezone context unavailable");
  }

  std::string ipText;
  if (wifiReady && platform::net::getLocalIp(ipText) && !ipText.empty()) {
    ESP_LOGI(kWifiTag, "connected ip=%s", ipText.c_str());
  }

  (void)timesync::ensureUtcTime();
  timesync::logUiTimeContext(geo.timezone.empty() ? nullptr : geo.timezone.c_str(),
                             geo.utcOffsetMinutes, geo.hasUtcOffset);
  boot::mark(baselineState, "geo_time_ready", kBaselineEnabled);

  ESP_LOGI(kBootTag, "idf scaffold ready");
  boot::mark(baselineState, "display_ready", kBaselineEnabled);
  ESP_LOGI(kBootTag, "setup complete");
  boot::mark(baselineState, "setup_complete", kBaselineEnabled);

  ESP_LOGI(kTag, "ESP-IDF shell running. Full app port in progress.");
  for (;;) {
    platform::sleepMs(30000);
    boot::markLoop(baselineState, wifiReady, kBaselineEnabled, kBaselineLoopPeriodMs);
  }
}
