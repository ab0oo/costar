// GeoLocation.cpp — IP-based geolocation fetch and JSON helpers.

#include "GeoLocation.h"

#include "HttpTransportGate.h"
#include "platform/Prefs.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr size_t kHttpTextMaxBytes = 8192U;
constexpr uint32_t kHttpTransportGateTimeoutMs = 7000U;

struct HttpTextResponse {
  int statusCode = 0;
  std::string body;
  std::string reason;
};

// ---------------------------------------------------------------------------
// Minimal JSON helpers (app_main-local, not shared with DSL runtime)
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// HTTP helper
// ---------------------------------------------------------------------------

bool httpGetText(const char* url, HttpTextResponse& out) {
  out = {};
  if (url == nullptr || *url == '\0') {
    out.reason = "url-empty";
    return false;
  }
  if (!http_transport_gate::take(kHttpTransportGateTimeoutMs)) {
    out.reason = "transport-gate-timeout";
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
    http_transport_gate::give();
    out.reason = "client-init";
    return false;
  }

  bool ok = false;
  esp_err_t err = esp_http_client_open(client, 0);
  if (err == ESP_OK) {
    const int64_t contentLength = esp_http_client_fetch_headers(client);
    out.statusCode = esp_http_client_get_status_code(client);
    out.body.clear();
    if (contentLength > 0) {
      if (static_cast<size_t>(contentLength) > kHttpTextMaxBytes) {
        out.reason = "body-too-large";
      } else {
        out.body.reserve(static_cast<size_t>(contentLength) + 1U);
      }
    } else {
      out.body.reserve(1024);
    }
    if (out.reason.empty()) {
      char buf[384];
      for (;;) {
        const int n = esp_http_client_read(client, buf, sizeof(buf));
        if (n > 0) {
          if (out.body.size() + static_cast<size_t>(n) > kHttpTextMaxBytes) {
            out.reason = "body-too-large";
            break;
          }
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
    }
  } else {
    out.reason = esp_err_to_name(err);
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  http_transport_gate::give();
  return ok;
}

// ---------------------------------------------------------------------------
// Geo parsing
// ---------------------------------------------------------------------------

bool parseGeoPayload(const std::string& body, GeoContext& outGeo, std::string& outLabel,
                     std::string* outCity = nullptr, std::string* outRegion = nullptr,
                     std::string* outCountry = nullptr) {
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
  if (!hasOffset) {
    double topLevelOffsetSeconds = 0;
    if (extractJsonNumber(body, "offset", topLevelOffsetSeconds)) {
      offsetMinutes = static_cast<int>(topLevelOffsetSeconds / 60.0);
      hasOffset = true;
    }
  }

  std::string city;
  std::string region;
  std::string country;
  (void)extractJsonString(body, "city", city);
  (void)extractJsonString(body, "region", region);
  if (region.empty()) {
    (void)extractJsonString(body, "regionName", region);
  }
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
  if (outCity != nullptr) {
    *outCity = city;
  }
  if (outRegion != nullptr) {
    *outRegion = region;
  }
  if (outCountry != nullptr) {
    *outCountry = country;
  }

  outGeo.lat = static_cast<float>(lat);
  outGeo.lon = static_cast<float>(lon);
  outGeo.timezone = tz;
  outGeo.utcOffsetMinutes = offsetMinutes;
  outGeo.hasUtcOffset = hasOffset;
  outGeo.hasLocation = true;
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool refreshGeoContextFromInternet(GeoContext& geoOut) {
  static constexpr std::array<const char*, 1> kGeoUrls = {
      // ip-api free endpoint is HTTP-only and includes timezone + UTC offset.
      "http://ip-api.com/json/?fields=status,message,lat,lon,city,regionName,country,timezone,offset",
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
    std::string city;
    std::string region;
    std::string country;
    GeoContext parsed;
    if (!parseGeoPayload(resp.body, parsed, label, &city, &region, &country)) {
      ESP_LOGW("geo", "parse fail source=%s body_len=%u", url,
               static_cast<unsigned>(resp.body.size()));
      continue;
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
    constexpr const char* kCachedCityKey = "city";
    constexpr const char* kCachedRegionKey = "region";
    constexpr const char* kCachedCountryKey = "country";
    constexpr int kModeAuto = 0;
    constexpr int kOffsetUnknown = -32768;
    (void)platform::prefs::putInt(kGeoNs, kModeKey, kModeAuto);
    (void)platform::prefs::putFloat(kGeoNs, kCachedLatKey, geoOut.lat);
    (void)platform::prefs::putFloat(kGeoNs, kCachedLonKey, geoOut.lon);
    (void)platform::prefs::putString(kGeoNs, kCachedTzKey, geoOut.timezone.c_str());
    (void)platform::prefs::putInt(kGeoNs, kCachedOffsetKey,
                                  geoOut.hasUtcOffset ? geoOut.utcOffsetMinutes : kOffsetUnknown);
    (void)platform::prefs::putString(kGeoNs, kCachedLabelKey, label.c_str());
    (void)platform::prefs::putString(kGeoNs, kCachedCityKey, city.c_str());
    (void)platform::prefs::putString(kGeoNs, kCachedRegionKey, region.c_str());
    (void)platform::prefs::putString(kGeoNs, kCachedCountryKey, country.c_str());
    return true;
  }
  return false;
}

bool isGeoValid(float lat, float lon, const std::string& timezone) {
  return !std::isnan(lat) && !std::isnan(lon) && !timezone.empty();
}
