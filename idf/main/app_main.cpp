#include "platform/Platform.h"
#include "RuntimeSettings.h"
#include "ConfigScreenEspIdf.h"
#include "DisplayBootstrapEspIdf.h"
#include "DisplaySpiEspIdf.h"
#include "LayoutRuntimeEspIdf.h"
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
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_crt_bundle.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
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
constexpr const char* kLayoutPrefsNs = "ui";
constexpr const char* kLayoutPrefsKey = "layout";
constexpr const char* kLayoutAPath = "/littlefs/screen_layout_a.json";
constexpr const char* kLayoutBPath = "/littlefs/screen_layout_b.json";
constexpr const char* kLayoutNytPath = "/littlefs/screen_layout_nyt.json";
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
  UiRect rowConfig;
  UiRect rowTouchCal;
};

enum class RuntimeMenuAction : uint8_t {
  None = 0,
  Toggle,
  SelectLayoutA,
  SelectLayoutB,
  SelectLayoutNyt,
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

struct GeoContext {
  float lat = NAN;
  float lon = NAN;
  std::string timezone;
  int utcOffsetMinutes = 0;
  bool hasUtcOffset = false;
  std::string source = "none";
  bool hasLocation = false;
};

struct HttpTextResponse {
  int statusCode = 0;
  std::string body;
  std::string reason;
};

bool parseJsonStringLiteral(const std::string& text, size_t startQuote, std::string& out) {
  if (startQuote >= text.size() || text[startQuote] != '"') {
    return false;
  }
  out.clear();
  out.reserve(64);
  bool esc = false;
  for (size_t i = startQuote + 1; i < text.size(); ++i) {
    const char c = text[i];
    if (esc) {
      out.push_back(c);
      esc = false;
      continue;
    }
    if (c == '\\') {
      esc = true;
      continue;
    }
    if (c == '"') {
      return true;
    }
    out.push_back(c);
  }
  return false;
}

bool findJsonKeyValueStart(const std::string& json, const char* key, size_t& valueStart) {
  if (key == nullptr || *key == '\0') {
    return false;
  }
  const std::string needle = std::string("\"") + key + "\"";
  size_t pos = json.find(needle);
  while (pos != std::string::npos) {
    size_t colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos) {
      return false;
    }
    size_t value = colon + 1;
    while (value < json.size() &&
           (json[value] == ' ' || json[value] == '\t' || json[value] == '\r' || json[value] == '\n')) {
      ++value;
    }
    if (value < json.size()) {
      valueStart = value;
      return true;
    }
    pos = json.find(needle, pos + needle.size());
  }
  return false;
}

bool extractJsonString(const std::string& json, const char* key, std::string& out) {
  size_t valueStart = 0;
  if (!findJsonKeyValueStart(json, key, valueStart)) {
    return false;
  }
  if (valueStart >= json.size() || json[valueStart] != '"') {
    return false;
  }
  return parseJsonStringLiteral(json, valueStart, out);
}

bool extractJsonNumber(const std::string& json, const char* key, double& out) {
  size_t valueStart = 0;
  if (!findJsonKeyValueStart(json, key, valueStart)) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const double value = std::strtod(json.c_str() + valueStart, &end);
  if (end == json.c_str() + valueStart || errno != 0) {
    return false;
  }
  out = value;
  return true;
}

bool extractNestedObjectValue(const std::string& json, const char* parentKey, const char* childKey,
                              std::string& out) {
  size_t valueStart = 0;
  if (!findJsonKeyValueStart(json, parentKey, valueStart)) {
    return false;
  }
  if (valueStart >= json.size() || json[valueStart] != '{') {
    return false;
  }
  const size_t objEnd = json.find('}', valueStart + 1);
  if (objEnd == std::string::npos || objEnd <= valueStart) {
    return false;
  }
  const std::string view = json.substr(valueStart, objEnd - valueStart + 1);
  return extractJsonString(view, childKey, out);
}

bool extractNestedObjectNumber(const std::string& json, const char* parentKey, const char* childKey,
                               double& out) {
  size_t valueStart = 0;
  if (!findJsonKeyValueStart(json, parentKey, valueStart)) {
    return false;
  }
  if (valueStart >= json.size() || json[valueStart] != '{') {
    return false;
  }
  const size_t objEnd = json.find('}', valueStart + 1);
  if (objEnd == std::string::npos || objEnd <= valueStart) {
    return false;
  }
  const std::string view = json.substr(valueStart, objEnd - valueStart + 1);
  return extractJsonNumber(view, childKey, out);
}

bool parseOffsetText(const std::string& rawText, int& outMinutes) {
  std::string raw = rawText;
  raw.erase(std::remove_if(raw.begin(), raw.end(), [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  }), raw.end());
  if (raw.empty()) {
    return false;
  }
  if (raw.rfind("UTC", 0) == 0) {
    raw = raw.substr(3);
  }
  if (raw.size() < 2) {
    return false;
  }
  const char sign = raw[0];
  if (sign != '+' && sign != '-') {
    return false;
  }
  int hh = 0;
  int mm = 0;
  if (raw.size() == 6 && raw[3] == ':') {  // +05:30
    hh = std::atoi(raw.substr(1, 2).c_str());
    mm = std::atoi(raw.substr(4, 2).c_str());
  } else if (raw.size() == 5) {  // +0530
    hh = std::atoi(raw.substr(1, 2).c_str());
    mm = std::atoi(raw.substr(3, 2).c_str());
  } else if (raw.size() == 3) {  // +05
    hh = std::atoi(raw.substr(1, 2).c_str());
    mm = 0;
  } else {
    return false;
  }
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
    return false;
  }
  int total = hh * 60 + mm;
  if (sign == '-') {
    total = -total;
  }
  outMinutes = total;
  return true;
}

bool httpGetText(const char* url, HttpTextResponse& out) {
  out = {};
  if (url == nullptr || *url == '\0') {
    out.reason = "url-empty";
    return false;
  }

  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.method = HTTP_METHOD_GET;
  cfg.timeout_ms = 8000;
  cfg.buffer_size = 1024;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.user_agent = "CoStar-IDF/1.0";

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) {
    out.reason = "client-init";
    return false;
  }

  bool ok = false;
  esp_err_t err = esp_http_client_open(client, 0);
  if (err == ESP_OK) {
    (void)esp_http_client_fetch_headers(client);
    out.statusCode = esp_http_client_get_status_code(client);
    out.body.clear();
    out.body.reserve(1024);
    char buf[384];
    for (;;) {
      const int n = esp_http_client_read(client, buf, sizeof(buf));
      if (n > 0) {
        out.body.append(buf, static_cast<size_t>(n));
        continue;
      }
      if (n == 0) {
        ok = true;
      } else {
        out.reason = "read";
      }
      break;
    }
  } else {
    out.reason = esp_err_to_name(err);
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return ok;
}

bool parseGeoPayload(const std::string& body, GeoContext& outGeo, std::string& outLabel) {
  double lat = NAN;
  double lon = NAN;
  bool hasLat = extractJsonNumber(body, "latitude", lat);
  bool hasLon = extractJsonNumber(body, "longitude", lon);
  if (!hasLat) {
    hasLat = extractJsonNumber(body, "lat", lat);
  }
  if (!hasLon) {
    hasLon = extractJsonNumber(body, "lon", lon);
  }
  if (!hasLat || !hasLon) {
    return false;
  }

  std::string tz;
  if (!extractJsonString(body, "timezone", tz)) {
    (void)extractNestedObjectValue(body, "timezone", "id", tz);
  }
  if (tz.empty()) {
    return false;
  }

  int offsetMinutes = 0;
  bool hasOffset = false;
  std::string offsetText;
  if (extractJsonString(body, "utc_offset", offsetText) && parseOffsetText(offsetText, offsetMinutes)) {
    hasOffset = true;
  }
  if (!hasOffset && extractNestedObjectValue(body, "timezone", "utc", offsetText) &&
      parseOffsetText(offsetText, offsetMinutes)) {
    hasOffset = true;
  }
  if (!hasOffset) {
    double tzOffsetSeconds = 0;
    if (extractNestedObjectNumber(body, "timezone", "offset", tzOffsetSeconds)) {
      offsetMinutes = static_cast<int>(tzOffsetSeconds / 60.0);
      hasOffset = true;
    }
  }
  if (!hasOffset) {
    double utcOffsetSeconds = 0;
    if (extractJsonNumber(body, "utc_offset_seconds", utcOffsetSeconds)) {
      offsetMinutes = static_cast<int>(utcOffsetSeconds / 60.0);
      hasOffset = true;
    }
  }

  std::string city;
  std::string region;
  std::string country;
  (void)extractJsonString(body, "city", city);
  (void)extractJsonString(body, "region", region);
  if (country.empty()) {
    (void)extractJsonString(body, "country", country);
  }
  if (country.empty()) {
    (void)extractJsonString(body, "country_name", country);
  }
  outLabel.clear();
  if (!city.empty()) {
    outLabel = city;
  }
  if (!region.empty()) {
    if (!outLabel.empty()) {
      outLabel += ", ";
    }
    outLabel += region;
  }
  if (!country.empty()) {
    if (!outLabel.empty()) {
      outLabel += ", ";
    }
    outLabel += country;
  }

  outGeo.lat = static_cast<float>(lat);
  outGeo.lon = static_cast<float>(lon);
  outGeo.timezone = tz;
  outGeo.utcOffsetMinutes = offsetMinutes;
  outGeo.hasUtcOffset = hasOffset;
  outGeo.hasLocation = true;
  return true;
}

bool fetchTimezoneOffsetMinutes(const std::string& timezone, int& outMinutes) {
  if (timezone.empty()) {
    return false;
  }
  const std::string url = "https://worldtimeapi.org/api/timezone/" + timezone;
  HttpTextResponse resp;
  if (!httpGetText(url.c_str(), resp)) {
    return false;
  }
  if (resp.statusCode < 200 || resp.statusCode >= 300) {
    return false;
  }
  std::string offsetText;
  if (!extractJsonString(resp.body, "utc_offset", offsetText)) {
    return false;
  }
  return parseOffsetText(offsetText, outMinutes);
}

bool refreshGeoContextFromInternet(GeoContext& geoOut) {
  static constexpr std::array<const char*, 4> kGeoUrls = {
      "https://ipwho.is/",
      "https://ipapi.co/json/",
      "https://ipinfo.io/json",
      "http://ip-api.com/json/",
  };

  for (const char* url : kGeoUrls) {
    HttpTextResponse resp;
    if (!httpGetText(url, resp)) {
      ESP_LOGW("geo", "fetch fail source=%s reason=%s", url, resp.reason.c_str());
      continue;
    }
    if (resp.statusCode < 200 || resp.statusCode >= 300) {
      ESP_LOGW("geo", "fetch fail source=%s status=%d", url, resp.statusCode);
      continue;
    }
    std::string label;
    GeoContext parsed;
    if (!parseGeoPayload(resp.body, parsed, label)) {
      ESP_LOGW("geo", "parse fail source=%s body_len=%u", url, static_cast<unsigned>(resp.body.size()));
      continue;
    }
    if (!parsed.hasUtcOffset) {
      int resolvedOffset = 0;
      if (fetchTimezoneOffsetMinutes(parsed.timezone, resolvedOffset)) {
        parsed.utcOffsetMinutes = resolvedOffset;
        parsed.hasUtcOffset = true;
        ESP_LOGI("geo", "timezone offset resolved from worldtimeapi tz=%s off_min=%d",
                 parsed.timezone.c_str(), parsed.utcOffsetMinutes);
      }
    }
    parsed.source = url;
    geoOut = parsed;
    constexpr const char* kGeoNs = "geo";
    constexpr const char* kModeKey = "mode";
    constexpr const char* kCachedLatKey = "lat";
    constexpr const char* kCachedLonKey = "lon";
    constexpr const char* kCachedTzKey = "tz";
    constexpr const char* kCachedOffsetKey = "off_min";
    constexpr const char* kCachedLabelKey = "label";
    constexpr int kModeAuto = 0;
    constexpr int kOffsetUnknown = -32768;
    (void)platform::prefs::putInt(kGeoNs, kModeKey, kModeAuto);
    (void)platform::prefs::putFloat(kGeoNs, kCachedLatKey, geoOut.lat);
    (void)platform::prefs::putFloat(kGeoNs, kCachedLonKey, geoOut.lon);
    (void)platform::prefs::putString(kGeoNs, kCachedTzKey, geoOut.timezone.c_str());
    (void)platform::prefs::putInt(kGeoNs, kCachedOffsetKey,
                                  geoOut.hasUtcOffset ? geoOut.utcOffsetMinutes : kOffsetUnknown);
    if (!label.empty()) {
      (void)platform::prefs::putString(kGeoNs, kCachedLabelKey, label.c_str());
    }
    return true;
  }
  return false;
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
  const uint16_t menuBtnW = 24;
  const uint16_t menuBtnH = 20;
  const uint16_t margin = 4;
  out.button = {static_cast<uint16_t>(w - menuBtnW - margin), margin, menuBtnW, menuBtnH};

  const uint16_t panelW = 160;
  const uint16_t rowH = 18;
  out.panel = {static_cast<uint16_t>(w - panelW - margin), static_cast<uint16_t>(out.button.y + out.button.h + 4),
               panelW, static_cast<uint16_t>(rowH * 5 + 6)};
  out.rowLayoutA = {static_cast<uint16_t>(out.panel.x + 3), static_cast<uint16_t>(out.panel.y + 3),
                    static_cast<uint16_t>(panelW - 6), rowH};
  out.rowLayoutB = {static_cast<uint16_t>(out.panel.x + 3), static_cast<uint16_t>(out.panel.y + 3 + rowH),
                    static_cast<uint16_t>(panelW - 6), rowH};
  out.rowLayoutNyt = {static_cast<uint16_t>(out.panel.x + 3), static_cast<uint16_t>(out.panel.y + 3 + rowH * 2),
                      static_cast<uint16_t>(panelW - 6), rowH};
  out.rowConfig = {static_cast<uint16_t>(out.panel.x + 3), static_cast<uint16_t>(out.panel.y + 3 + rowH * 3),
                   static_cast<uint16_t>(panelW - 6), rowH};
  out.rowTouchCal = {static_cast<uint16_t>(out.panel.x + 3), static_cast<uint16_t>(out.panel.y + 3 + rowH * 4),
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
  (void)display_spi::fillRect(r.rowLayoutA.x, r.rowLayoutA.y, r.rowLayoutA.w, r.rowLayoutA.h,
                              layoutAActive ? rowActive : rowBg);
  (void)display_spi::fillRect(r.rowLayoutB.x, r.rowLayoutB.y, r.rowLayoutB.w, r.rowLayoutB.h,
                              layoutBActive ? rowActive : rowBg);
  (void)display_spi::fillRect(r.rowLayoutNyt.x, r.rowLayoutNyt.y, r.rowLayoutNyt.w, r.rowLayoutNyt.h,
                              layoutNytActive ? rowActive : rowBg);
  (void)display_spi::fillRect(r.rowConfig.x, r.rowConfig.y, r.rowConfig.w, r.rowConfig.h, rowBg);
  (void)display_spi::fillRect(r.rowTouchCal.x, r.rowTouchCal.y, r.rowTouchCal.w, r.rowTouchCal.h, rowBg);

  drawTinyText(r.rowLayoutA.x + 4, r.rowLayoutA.y + 5, "Layout A (HA)", rowText,
               layoutAActive ? rowActive : rowBg, 1);
  drawTinyText(r.rowLayoutB.x + 4, r.rowLayoutB.y + 5, "Layout B (WX)", rowText,
               layoutBActive ? rowActive : rowBg, 1);
  drawTinyText(r.rowLayoutNyt.x + 4, r.rowLayoutNyt.y + 5, "Layout C (NYT)", rowText,
               layoutNytActive ? rowActive : rowBg, 1);
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
  if (path != kLayoutAPath && path != kLayoutBPath && path != kLayoutNytPath) {
    return kLayoutAPath;
  }
  return path;
}

void savePreferredLayoutPath(const std::string& path) {
  if (path != kLayoutAPath && path != kLayoutBPath && path != kLayoutNytPath) {
    return;
  }
  (void)platform::prefs::putString(kLayoutPrefsNs, kLayoutPrefsKey, path.c_str());
}

void drawCalibrationTarget(uint16_t x, uint16_t y, uint16_t color) {
  const uint16_t dot = 8;
  const uint16_t arm = 16;
  const uint16_t halfDot = dot / 2;
  (void)display_spi::fillRect(static_cast<uint16_t>(x - halfDot), static_cast<uint16_t>(y - 1), dot, 3,
                              color);
  (void)display_spi::fillRect(static_cast<uint16_t>(x - 1), static_cast<uint16_t>(y - halfDot), 3, dot,
                              color);
  (void)display_spi::fillRect(static_cast<uint16_t>(x - 1), static_cast<uint16_t>(y - arm), 3,
                              static_cast<uint16_t>(arm * 2), color);
  (void)display_spi::fillRect(static_cast<uint16_t>(x - arm), static_cast<uint16_t>(y - 1),
                              static_cast<uint16_t>(arm * 2), 3, color);
}

void showPostCalibrationColorCheck() {
  const uint16_t w = display_spi::width();
  const uint16_t h = display_spi::height();
  if (w < 40 || h < 40) {
    return;
  }
  const uint16_t halfW = w / 2;
  const uint16_t halfH = h / 2;
  const uint16_t cBlack = 0x0000;
  const uint16_t cWhite = 0xFFFF;
  const uint16_t cRed = 0xF800;
  const uint16_t cGreen = 0x07E0;
  const uint16_t cBlue = 0x001F;

  (void)display_spi::fillRect(0, 0, halfW, halfH, cBlack);
  (void)display_spi::fillRect(halfW, 0, static_cast<uint16_t>(w - halfW), halfH, cWhite);

  const uint16_t lowerY = halfH;
  const uint16_t lowerH = static_cast<uint16_t>(h - halfH);
  const uint16_t third = w / 3;
  (void)display_spi::fillRect(0, lowerY, third, lowerH, cRed);
  (void)display_spi::fillRect(third, lowerY, third, lowerH, cGreen);
  (void)display_spi::fillRect(static_cast<uint16_t>(third * 2), lowerY,
                              static_cast<uint16_t>(w - third * 2), lowerH, cBlue);

  ESP_LOGI(kTouchTag, "post-calibration color check shown (TL black, TR white, RGB bottom)");
  platform::sleepMs(1200);
}

void drawDisplayModePattern(bool bgr, bool invert) {
  const uint16_t w = display_spi::width();
  const uint16_t h = display_spi::height();
  if (w < 40 || h < 40) {
    return;
  }
  const uint16_t cBlack = 0x0000;
  const uint16_t cWhite = 0xFFFF;
  const uint16_t cRed = 0xF800;
  const uint16_t cGreen = 0x07E0;
  const uint16_t cBlue = 0x001F;
  const uint16_t cYellow = 0xFFE0;

  (void)display_spi::clear(0x0000);
  const uint16_t halfW = w / 2;
  const uint16_t topH = h / 3;
  const uint16_t midY = topH;
  const uint16_t midH = h / 3;
  const uint16_t botY = static_cast<uint16_t>(topH + midH);
  const uint16_t botH = static_cast<uint16_t>(h - botY);

  (void)display_spi::fillRect(0, 0, halfW, topH, cBlack);
  (void)display_spi::fillRect(halfW, 0, static_cast<uint16_t>(w - halfW), topH, cWhite);

  const uint16_t quarter = w / 4;
  (void)display_spi::fillRect(0, midY, quarter, midH, cRed);
  (void)display_spi::fillRect(quarter, midY, quarter, midH, cGreen);
  (void)display_spi::fillRect(static_cast<uint16_t>(quarter * 2), midY, quarter, midH, cBlue);
  (void)display_spi::fillRect(static_cast<uint16_t>(quarter * 3), midY,
                              static_cast<uint16_t>(w - quarter * 3), midH, cYellow);

  const uint16_t leftColor = bgr ? 0x001F : 0x07E0;
  const uint16_t rightColor = invert ? 0xF800 : 0x07E0;
  (void)display_spi::fillRect(0, botY, halfW, botH, leftColor);
  (void)display_spi::fillRect(halfW, botY, static_cast<uint16_t>(w - halfW), botH, rightColor);
}

bool runDisplayModeCalibrationIfNeeded() {
  constexpr const char* kDisplayPrefsNs = "display";
  constexpr const char* kColorSetKey = "color_set";
  constexpr const char* kColorBgrKey = "color_bgr";
  constexpr const char* kInvertSetKey = "inv_set";
  constexpr const char* kInvertOnKey = "inv_on";

  const bool haveColor = platform::prefs::getBool(kDisplayPrefsNs, kColorSetKey, false);
  const bool haveInvert = platform::prefs::getBool(kDisplayPrefsNs, kInvertSetKey, false);

  bool bgr = platform::prefs::getBool(kDisplayPrefsNs, kColorBgrKey, false);
  bool invert = platform::prefs::getBool(kDisplayPrefsNs, kInvertOnKey, true);
  if (!display_spi::applyPanelTuning(bgr, invert, false)) {
    return false;
  }

  if (haveColor && haveInvert) {
    ESP_LOGI(kTouchTag, "display mode already calibrated; using saved bgr=%d invert=%d",
             bgr ? 1 : 0, invert ? 1 : 0);
    return true;
  }

  drawDisplayModePattern(bgr, invert);
  ESP_LOGW(kTouchTag,
           "display mode calibration: tap LEFT half toggles RGB/BGR, RIGHT half toggles invert, "
           "BOTTOM third saves");

  if (!AppConfig::kTouchEnabled) {
    (void)display_spi::applyPanelTuning(bgr, invert, true);
    return true;
  }

  const uint32_t start = platform::millisMs();
  bool held = false;
  while (platform::millisMs() - start < 45000U) {
    touch_input::Point p;
    if (!touch_input::read(p)) {
      held = false;
      platform::sleepMs(15);
      continue;
    }
    if (held) {
      platform::sleepMs(25);
      continue;
    }
    held = true;
    const uint16_t h = display_spi::height();
    const uint16_t w = display_spi::width();
    if (p.y >= (h * 2U) / 3U) {
      (void)display_spi::applyPanelTuning(bgr, invert, true);
      ESP_LOGI(kTouchTag, "display mode saved bgr=%d invert=%d", bgr ? 1 : 0, invert ? 1 : 0);
      (void)display_spi::clear(0x0000);
      return true;
    }
    if (p.x < (w / 2U)) {
      bgr = !bgr;
    } else {
      invert = !invert;
    }
    (void)display_spi::applyPanelTuning(bgr, invert, false);
    drawDisplayModePattern(bgr, invert);
    ESP_LOGI(kTouchTag, "display mode trial bgr=%d invert=%d", bgr ? 1 : 0, invert ? 1 : 0);
  }

  (void)display_spi::applyPanelTuning(bgr, invert, true);
  ESP_LOGW(kTouchTag, "display mode calibration timeout; saved current bgr=%d invert=%d", bgr ? 1 : 0,
           invert ? 1 : 0);
  (void)display_spi::clear(0x0000);
  return true;
}

bool captureCalibrationPoint(uint16_t targetX, uint16_t targetY, uint16_t& rawX, uint16_t& rawY,
                             bool requireNearTarget) {
  const uint16_t cBg = 0x0000;
  const uint16_t cTarget = 0xFFFF;

  (void)display_spi::clear(cBg);
  drawCalibrationTarget(targetX, targetY, cTarget);

  const uint32_t timeoutMs = 20000;
  const uint32_t start = platform::millisMs();
  // Require release before starting this corner capture to avoid carrying
  // a lingering previous press into the next point.
  while (platform::millisMs() - start < 1500U) {
    touch_input::Point p;
    if (!touch_input::read(p)) {
      break;
    }
    platform::sleepMs(12);
  }

  uint32_t sumX = 0;
  uint32_t sumY = 0;
  uint16_t count = 0;
  bool touching = false;
  uint16_t minRawX = 0xFFFF;
  uint16_t maxRawX = 0;
  uint16_t minRawY = 0xFFFF;
  uint16_t maxRawY = 0;
  constexpr uint16_t kMinSamples = 8;
  constexpr uint16_t kMaxRawJitter = 180;
  constexpr int32_t kNearRadiusPx = 40;

  while (platform::millisMs() - start < timeoutMs) {
    touch_input::Point p;
    if (touch_input::read(p)) {
      touching = true;
      if (requireNearTarget) {
        const int32_t dx = static_cast<int32_t>(p.x) - static_cast<int32_t>(targetX);
        const int32_t dy = static_cast<int32_t>(p.y) - static_cast<int32_t>(targetY);
        if (dx < -kNearRadiusPx || dx > kNearRadiusPx || dy < -kNearRadiusPx ||
            dy > kNearRadiusPx) {
          platform::sleepMs(20);
          continue;
        }
      }
      if (count < 24) {
        sumX += p.rawX;
        sumY += p.rawY;
        minRawX = std::min<uint16_t>(minRawX, p.rawX);
        maxRawX = std::max<uint16_t>(maxRawX, p.rawX);
        minRawY = std::min<uint16_t>(minRawY, p.rawY);
        maxRawY = std::max<uint16_t>(maxRawY, p.rawY);
        ++count;
      }
      if (requireNearTarget) {
        config_screen::markTouch(p.x, p.y);
      }
      platform::sleepMs(20);
      continue;
    }

    if (touching) {
      const uint16_t jitterX = static_cast<uint16_t>(maxRawX - minRawX);
      const uint16_t jitterY = static_cast<uint16_t>(maxRawY - minRawY);
      if (count >= kMinSamples && jitterX <= kMaxRawJitter && jitterY <= kMaxRawJitter) {
        rawX = static_cast<uint16_t>(sumX / count);
        rawY = static_cast<uint16_t>(sumY / count);
        platform::sleepMs(120);
        return true;
      }
      touching = false;
      sumX = 0;
      sumY = 0;
      count = 0;
      minRawX = 0xFFFF;
      maxRawX = 0;
      minRawY = 0xFFFF;
      maxRawY = 0;
    }
    platform::sleepMs(12);
  }
  return false;
}

bool runTouchCalibration(bool force) {
  touch_input::Calibration cal = {};
  if (!force) {
    if (touch_input::loadCalibration(cal)) {
      ESP_LOGI(kTouchTag, "cal loaded minX=%u maxX=%u minY=%u maxY=%u invX=%d invY=%d", cal.rawMinX,
               cal.rawMaxX, cal.rawMinY, cal.rawMaxY, cal.invertX ? 1 : 0, cal.invertY ? 1 : 0);
      return true;
    }
  } else if (touch_input::loadCalibration(cal)) {
    ESP_LOGI(kTouchTag, "forcing calibration over stored minX=%u maxX=%u minY=%u maxY=%u",
             cal.rawMinX, cal.rawMaxX, cal.rawMinY, cal.rawMaxY);
  }

  ESP_LOGW(kTouchTag, "no persisted calibration; entering calibration");
  const uint16_t w = AppConfig::kScreenWidth;
  const uint16_t h = AppConfig::kScreenHeight;
  const uint16_t m = 24;
  const uint16_t ulX = m;
  const uint16_t ulY = m;
  const uint16_t urX = static_cast<uint16_t>(w - 1 - m);
  const uint16_t urY = m;
  const uint16_t llX = m;
  const uint16_t llY = static_cast<uint16_t>(h - 1 - m);
  const uint16_t lrX = static_cast<uint16_t>(w - 1 - m);
  const uint16_t lrY = static_cast<uint16_t>(h - 1 - m);

  auto solveCalibration = [&](uint16_t ulRawX, uint16_t ulRawY, uint16_t urRawX, uint16_t urRawY,
                              uint16_t llRawX, uint16_t llRawY, uint16_t lrRawX, uint16_t lrRawY,
                              touch_input::Calibration& calibrated) -> bool {
    const int32_t horizDxX = std::abs(static_cast<int32_t>(urRawX) - static_cast<int32_t>(ulRawX)) +
                             std::abs(static_cast<int32_t>(lrRawX) - static_cast<int32_t>(llRawX));
    const int32_t horizDxY = std::abs(static_cast<int32_t>(urRawY) - static_cast<int32_t>(ulRawY)) +
                             std::abs(static_cast<int32_t>(lrRawY) - static_cast<int32_t>(llRawY));
    const bool swapXY = horizDxY > horizDxX;

    const uint16_t srcUlX = swapXY ? ulRawY : ulRawX;
    const uint16_t srcUrX = swapXY ? urRawY : urRawX;
    const uint16_t srcLlX = swapXY ? llRawY : llRawX;
    const uint16_t srcLrX = swapXY ? lrRawY : lrRawX;
    const uint16_t srcUlY = swapXY ? ulRawX : ulRawY;
    const uint16_t srcUrY = swapXY ? urRawX : urRawY;
    const uint16_t srcLlY = swapXY ? llRawX : llRawY;
    const uint16_t srcLrY = swapXY ? lrRawX : lrRawY;

    const uint16_t minRawX = std::min(std::min(srcUlX, srcUrX), std::min(srcLlX, srcLrX));
    const uint16_t maxRawX = std::max(std::max(srcUlX, srcUrX), std::max(srcLlX, srcLrX));
    const uint16_t minRawY = std::min(std::min(srcUlY, srcUrY), std::min(srcLlY, srcLrY));
    const uint16_t maxRawY = std::max(std::max(srcUlY, srcUrY), std::max(srcLlY, srcLrY));
    const uint16_t spanX = static_cast<uint16_t>(maxRawX - minRawX);
    const uint16_t spanY = static_cast<uint16_t>(maxRawY - minRawY);
    if (spanX < 600 || spanY < 600) {
      return false;
    }

    // Corner targets are inset by "m" pixels, so the measured raw min/max
    // correspond to x=m..(w-1-m), y=m..(h-1-m). Extrapolate to true screen
    // edges so mapped touch coordinates don't clip inward by ~m pixels.
    const int32_t mPx = 24;
    const int32_t wPx = static_cast<int32_t>(AppConfig::kScreenWidth);
    const int32_t hPx = static_cast<int32_t>(AppConfig::kScreenHeight);
    const int32_t innerW = wPx - 1 - 2 * mPx;
    const int32_t innerH = hPx - 1 - 2 * mPx;
    if (innerW <= 0 || innerH <= 0) {
      return false;
    }

    const int32_t rawSpanX = static_cast<int32_t>(maxRawX) - static_cast<int32_t>(minRawX);
    const int32_t rawSpanY = static_cast<int32_t>(maxRawY) - static_cast<int32_t>(minRawY);
    const int32_t edgePadRawX = (rawSpanX * mPx) / innerW;
    const int32_t edgePadRawY = (rawSpanY * mPx) / innerH;

    int32_t effectiveMinX = static_cast<int32_t>(minRawX) - edgePadRawX;
    int32_t effectiveMaxX = static_cast<int32_t>(maxRawX) + edgePadRawX;
    int32_t effectiveMinY = static_cast<int32_t>(minRawY) - edgePadRawY;
    int32_t effectiveMaxY = static_cast<int32_t>(maxRawY) + edgePadRawY;

    effectiveMinX = std::max<int32_t>(0, effectiveMinX);
    effectiveMinY = std::max<int32_t>(0, effectiveMinY);
    effectiveMaxX = std::min<int32_t>(4095, effectiveMaxX);
    effectiveMaxY = std::min<int32_t>(4095, effectiveMaxY);

    calibrated.rawMinX = static_cast<uint16_t>(effectiveMinX);
    calibrated.rawMaxX = static_cast<uint16_t>(effectiveMaxX);
    calibrated.rawMinY = static_cast<uint16_t>(effectiveMinY);
    calibrated.rawMaxY = static_cast<uint16_t>(effectiveMaxY);
    calibrated.swapXY = swapXY;
    const uint32_t leftAvgX = (static_cast<uint32_t>(srcUlX) + srcLlX) / 2U;
    const uint32_t rightAvgX = (static_cast<uint32_t>(srcUrX) + srcLrX) / 2U;
    const uint32_t topAvgY = (static_cast<uint32_t>(srcUlY) + srcUrY) / 2U;
    const uint32_t bottomAvgY = (static_cast<uint32_t>(srcLlY) + srcLrY) / 2U;
    calibrated.invertX = leftAvgX > rightAvgX;
    calibrated.invertY = topAvgY > bottomAvgY;
    calibrated.xCorrLeft = 0;
    calibrated.xCorrRight = 0;
    calibrated.yCorr = 0;
    return true;
  };

  touch_input::Calibration pass1Cal = {};
  touch_input::Calibration pass2Cal = {};
  bool pass1Ok = false;
  bool pass2Ok = false;

  for (int pass = 1; pass <= 2; ++pass) {
    const bool requireNearTarget = (pass == 2);
    bool passSolved = false;
    for (int attempt = 1; attempt <= 2; ++attempt) {
      uint16_t ulRawX = 0, ulRawY = 0;
      uint16_t urRawX = 0, urRawY = 0;
      uint16_t llRawX = 0, llRawY = 0;
      uint16_t lrRawX = 0, lrRawY = 0;
      if (!captureCalibrationPoint(ulX, ulY, ulRawX, ulRawY, requireNearTarget) ||
          !captureCalibrationPoint(urX, urY, urRawX, urRawY, requireNearTarget) ||
          !captureCalibrationPoint(llX, llY, llRawX, llRawY, requireNearTarget) ||
          !captureCalibrationPoint(lrX, lrY, lrRawX, lrRawY, requireNearTarget)) {
        ESP_LOGE(kTouchTag, "cal capture timeout pass=%d attempt=%d", pass, attempt);
        continue;
      }
      ESP_LOGI(kTouchTag,
               "cal raw pass=%d attempt=%d UL=(%u,%u) UR=(%u,%u) LL=(%u,%u) LR=(%u,%u)", pass,
               attempt, ulRawX, ulRawY, urRawX, urRawY, llRawX, llRawY, lrRawX, lrRawY);

      touch_input::Calibration solved = {};
      if (!solveCalibration(ulRawX, ulRawY, urRawX, urRawY, llRawX, llRawY, lrRawX, lrRawY,
                            solved)) {
        ESP_LOGE(kTouchTag, "cal spans invalid pass=%d attempt=%d", pass, attempt);
        continue;
      }

      if (pass == 1) {
        pass1Cal = solved;
        pass1Ok = true;
        touch_input::setCalibration(pass1Cal);
        ESP_LOGI(kTouchTag,
                 "cal pass1 solved minX=%u maxX=%u minY=%u maxY=%u swap=%d invX=%d invY=%d",
                 pass1Cal.rawMinX, pass1Cal.rawMaxX, pass1Cal.rawMinY, pass1Cal.rawMaxY,
                 pass1Cal.swapXY ? 1 : 0, pass1Cal.invertX ? 1 : 0, pass1Cal.invertY ? 1 : 0);
      } else {
        pass2Cal = solved;
        pass2Cal.xCorrLeft = 0;
        pass2Cal.xCorrRight = 0;
        pass2Cal.yCorr = 0;
        pass2Ok = true;
        ESP_LOGI(kTouchTag,
                 "cal pass2 solved minX=%u maxX=%u minY=%u maxY=%u swap=%d invX=%d invY=%d "
                 "xCorrL=%d xCorrR=%d yCorr=%d",
                 pass2Cal.rawMinX, pass2Cal.rawMaxX, pass2Cal.rawMinY, pass2Cal.rawMaxY,
                 pass2Cal.swapXY ? 1 : 0, pass2Cal.invertX ? 1 : 0, pass2Cal.invertY ? 1 : 0,
                 static_cast<int>(pass2Cal.xCorrLeft), static_cast<int>(pass2Cal.xCorrRight),
                 static_cast<int>(pass2Cal.yCorr));
      }
      passSolved = true;
      break;
    }
    if (!passSolved) {
      ESP_LOGE(kTouchTag, "calibration pass %d failed", pass);
      if (pass == 1) {
        break;
      }
    }
  }

  if (pass2Ok) {
    touch_input::setCalibration(pass2Cal);
    if (!touch_input::saveCalibration(pass2Cal)) {
      ESP_LOGW(kTouchTag, "failed to persist pass2 calibration");
    }
    ESP_LOGI(kTouchTag, "cal saved pass2 minX=%u maxX=%u minY=%u maxY=%u swap=%d invX=%d invY=%d",
             pass2Cal.rawMinX, pass2Cal.rawMaxX, pass2Cal.rawMinY, pass2Cal.rawMaxY,
             pass2Cal.swapXY ? 1 : 0, pass2Cal.invertX ? 1 : 0, pass2Cal.invertY ? 1 : 0);
    showPostCalibrationColorCheck();
    (void)display_spi::clear(0x0000);
    return true;
  }
  if (pass1Ok) {
    touch_input::setCalibration(pass1Cal);
    if (!touch_input::saveCalibration(pass1Cal)) {
      ESP_LOGW(kTouchTag, "failed to persist pass1 calibration fallback");
    }
    ESP_LOGW(kTouchTag,
             "cal saved pass1 fallback minX=%u maxX=%u minY=%u maxY=%u swap=%d invX=%d invY=%d",
             pass1Cal.rawMinX, pass1Cal.rawMaxX, pass1Cal.rawMinY, pass1Cal.rawMaxY,
             pass1Cal.swapXY ? 1 : 0, pass1Cal.invertX ? 1 : 0, pass1Cal.invertY ? 1 : 0);
    showPostCalibrationColorCheck();
    (void)display_spi::clear(0x0000);
    return true;
  }

  ESP_LOGE(kTouchTag, "calibration failed after retries; using defaults");
  return false;
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
      {"dsl_weather_now", "/littlefs/dsl_active/weather_now.json"},
      {"dsl_forecast", "/littlefs/dsl_active/forecast.json"},
      {"dsl_clock_analog_full", "/littlefs/dsl_active/clock_analog_full.json"},
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

bool isGeoValid(float lat, float lon, const std::string& timezone) {
  return !std::isnan(lat) && !std::isnan(lon) && !timezone.empty();
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
    ESP_LOGW("geo", "cache/override missing; timezone context unavailable");
  }

  std::string ipText;
  if (wifiReady && platform::net::getLocalIp(ipText) && !ipText.empty()) {
    ESP_LOGI(kWifiTag, "connected ip=%s", ipText.c_str());
  }

  if (wifiReady && geo.source != "manual") {
    GeoContext refreshedGeo;
    if (refreshGeoContextFromInternet(refreshedGeo)) {
      geo = refreshedGeo;
      ESP_LOGI("geo", "online source=%s lat=%.4f lon=%.4f tz=%s off_min=%d known=%d",
               geo.source.c_str(), geo.lat, geo.lon, geo.timezone.c_str(), geo.utcOffsetMinutes,
               geo.hasUtcOffset ? 1 : 0);
    } else {
      ESP_LOGW("geo", "online fetch failed; using cached timezone context");
    }
  }

  (void)timesync::ensureUtcTime();
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

  ESP_LOGI(kTag, "runtime task pinned core=1; main task idling on core=%d", static_cast<int>(xPortGetCoreID()));
  for (;;) {
    platform::sleepMs(1000);
  }
}
