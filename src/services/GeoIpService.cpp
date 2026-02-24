#include "services/GeoIpService.h"

#include <ArduinoJson.h>

#include "RuntimeGeo.h"
#include "platform/Fs.h"
#include "platform/Net.h"
#include "platform/Prefs.h"
#include "platform/Platform.h"

namespace {
constexpr char kPrefsNs[] = "geo";
constexpr char kLatKey[] = "lat";
constexpr char kLonKey[] = "lon";
constexpr char kTzKey[] = "tz";
constexpr char kLabelKey[] = "label";
constexpr char kOffsetKey[] = "off_min";
constexpr int kOffsetUnknown = -32768;
constexpr char kModeKey[] = "mode";
constexpr char kManualLatKey[] = "mlat";
constexpr char kManualLonKey[] = "mlon";
constexpr char kManualTzKey[] = "mtz";
constexpr char kManualOffsetKey[] = "moff";
constexpr char kManualLabelKey[] = "mlabel";
constexpr char kManualCityKey[] = "mcity";
constexpr int kModeAuto = 0;
constexpr int kModeManual = 1;
constexpr char kManualSsidPath[] = "/geo_manual_by_ssid.json";
constexpr char kEntriesKey[] = "entries";
constexpr char kSsidKey[] = "ssid";
constexpr char kHasOffsetKey[] = "has_offset";
constexpr char kCityKey[] = "city";

constexpr char kGeoUrlPrimary[] = "https://ipwho.is/";
constexpr char kGeoUrlFallback[] = "https://ipapi.co/json/";
constexpr char kGeoUrlFallback2[] = "https://ipinfo.io/json";
constexpr char kGeoUrlFallback3[] = "http://ip-api.com/json/";

String urlEncode(const String& input) {
  String out;
  out.reserve(input.length() * 3);
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else if (c == ' ') {
      out += "%20";
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
      out += buf;
    }
  }
  return out;
}
}  // namespace

void GeoIpService::setError(const String& msg) {
  lastError_ = msg;
}

bool GeoIpService::loadOverride() {
  const String ssid = currentWifiSsid();
  float lat = NAN;
  float lon = NAN;
  String tz;
  int offMin = 0;
  bool hasOffset = false;
  String label;
  String city;
  if (loadManualForSsid(ssid, lat, lon, tz, offMin, hasOffset, label, city)) {
    String resolvedLabel = label;
    if (resolvedLabel.isEmpty()) {
      resolvedLabel = city;
    }
    if (resolvedLabel.isEmpty()) {
      resolvedLabel = String(lat, 4) + "," + String(lon, 4);
    }
    RuntimeGeo::setLocation(lat, lon, tz, hasOffset ? offMin : 0, hasOffset, resolvedLabel);
    lastSource_ = "manual";
    setError("");
    return true;
  }

  const int mode = platform::prefs::getInt(kPrefsNs, kModeKey, kModeAuto);
  lat = platform::prefs::getFloat(kPrefsNs, kManualLatKey, NAN);
  lon = platform::prefs::getFloat(kPrefsNs, kManualLonKey, NAN);
  tz = String(platform::prefs::getString(kPrefsNs, kManualTzKey, "").c_str());
  label = String(platform::prefs::getString(kPrefsNs, kManualLabelKey, "").c_str());
  offMin = platform::prefs::getInt(kPrefsNs, kManualOffsetKey, kOffsetUnknown);

  if (mode != kModeManual || isnan(lat) || isnan(lon) || tz.isEmpty()) {
    setError("manual override missing");
    return false;
  }

  hasOffset = offMin != kOffsetUnknown;
  RuntimeGeo::setLocation(lat, lon, tz, hasOffset ? offMin : 0, hasOffset, label);
  lastSource_ = "manual";
  setError("");
  return true;
}

bool GeoIpService::loadCached() {
  const float lat = platform::prefs::getFloat(kPrefsNs, kLatKey, NAN);
  const float lon = platform::prefs::getFloat(kPrefsNs, kLonKey, NAN);
  const String tz = String(platform::prefs::getString(kPrefsNs, kTzKey, "").c_str());
  const String label = String(platform::prefs::getString(kPrefsNs, kLabelKey, "").c_str());
  const int offMin = platform::prefs::getInt(kPrefsNs, kOffsetKey, kOffsetUnknown);

  if (isnan(lat) || isnan(lon) || tz.isEmpty()) {
    setError("cache missing lat/lon/tz");
    return false;
  }

  const bool hasOffset = offMin != kOffsetUnknown;
  RuntimeGeo::setLocation(lat, lon, tz, hasOffset ? offMin : 0, hasOffset, label);
  lastSource_ = "nvs-cache";
  setError("");
  return true;
}

bool GeoIpService::saveCached(float lat, float lon, const String& tz, const String& label) {
  platform::prefs::putFloat(kPrefsNs, kLatKey, lat);
  platform::prefs::putFloat(kPrefsNs, kLonKey, lon);
  platform::prefs::putString(kPrefsNs, kTzKey, tz.c_str());
  if (!label.isEmpty()) {
    platform::prefs::putString(kPrefsNs, kLabelKey, label.c_str());
  }
  if (RuntimeGeo::hasUtcOffset) {
    platform::prefs::putInt(kPrefsNs, kOffsetKey, RuntimeGeo::utcOffsetMinutes);
  }
  return true;
}

bool GeoIpService::saveManual(float lat, float lon, const String& tz, int offsetMinutes,
                              bool hasOffset, const String& label, const String& city) {
  const String ssid = currentWifiSsid();
  const bool hasSsid = !ssid.isEmpty();

  platform::prefs::putInt(kPrefsNs, kModeKey, hasSsid ? kModeAuto : kModeManual);
  platform::prefs::putFloat(kPrefsNs, kManualLatKey, lat);
  platform::prefs::putFloat(kPrefsNs, kManualLonKey, lon);
  platform::prefs::putString(kPrefsNs, kManualTzKey, tz.c_str());
  if (hasOffset) {
    platform::prefs::putInt(kPrefsNs, kManualOffsetKey, offsetMinutes);
  } else {
    platform::prefs::putInt(kPrefsNs, kManualOffsetKey, kOffsetUnknown);
  }
  if (!label.isEmpty()) {
    platform::prefs::putString(kPrefsNs, kManualLabelKey, label.c_str());
  }
  if (!city.isEmpty()) {
    platform::prefs::putString(kPrefsNs, kManualCityKey, city.c_str());
  }

  if (hasSsid) {
    saveManualForSsid(ssid, lat, lon, tz, offsetMinutes, hasOffset, label, city);
  }
  return true;
}

bool GeoIpService::clearOverride() {
  platform::prefs::putInt(kPrefsNs, kModeKey, kModeAuto);

  const String ssid = currentWifiSsid();
  if (!ssid.isEmpty()) {
    clearManualForSsid(ssid);
  }

  lastSource_ = "auto";
  setError("");
  return true;
}

String GeoIpService::currentWifiSsid() const {
  std::string ssidRaw;
  if (!platform::net::getSsid(ssidRaw)) {
    return String();
  }
  String ssid = String(ssidRaw.c_str());
  ssid.trim();
  return ssid;
}

bool GeoIpService::loadManualForSsid(const String& ssid, float& lat, float& lon, String& tz,
                                     int& offsetMinutes, bool& hasOffset, String& label,
                                     String& city) const {
  if (ssid.isEmpty()) {
    return false;
  }
  if (!platform::fs::exists(kManualSsidPath)) {
    return false;
  }

  platform::fs::File f = platform::fs::open(kManualSsidPath, FILE_READ);
  if (!f || f.isDirectory()) {
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    return false;
  }

  JsonArrayConst entries = doc[kEntriesKey].as<JsonArrayConst>();
  if (entries.isNull()) {
    return false;
  }

  for (JsonObjectConst entry : entries) {
    const String entrySsid = entry[kSsidKey] | String();
    if (entrySsid != ssid) {
      continue;
    }

    lat = entry[kManualLatKey] | NAN;
    lon = entry[kManualLonKey] | NAN;
    tz = entry[kManualTzKey] | String();
    label = entry[kManualLabelKey] | String();
    city = entry[kCityKey] | String();

    hasOffset = entry[kHasOffsetKey] | false;
    if (hasOffset) {
      offsetMinutes = entry[kManualOffsetKey] | 0;
    } else {
      const int offRaw = entry[kManualOffsetKey] | kOffsetUnknown;
      if (offRaw != kOffsetUnknown) {
        offsetMinutes = offRaw;
        hasOffset = true;
      } else {
        offsetMinutes = 0;
      }
    }

    if (isnan(lat) || isnan(lon) || tz.isEmpty()) {
      return false;
    }
    return true;
  }

  return false;
}

bool GeoIpService::saveManualForSsid(const String& ssid, float lat, float lon, const String& tz,
                                     int offsetMinutes, bool hasOffset, const String& label,
                                     const String& city) const {
  if (ssid.isEmpty()) {
    return false;
  }

  JsonDocument doc;
  if (platform::fs::exists(kManualSsidPath)) {
    platform::fs::File in = platform::fs::open(kManualSsidPath, FILE_READ);
    if (in && !in.isDirectory()) {
      deserializeJson(doc, in);
      in.close();
    } else if (in) {
      in.close();
    }
  }

  JsonArray entries = doc[kEntriesKey].as<JsonArray>();
  if (entries.isNull()) {
    entries = doc[kEntriesKey].to<JsonArray>();
  }

  JsonObject target;
  for (JsonObject entry : entries) {
    const String entrySsid = entry[kSsidKey] | String();
    if (entrySsid == ssid) {
      target = entry;
      break;
    }
  }
  if (target.isNull()) {
    target = entries.add<JsonObject>();
  }

  target[kSsidKey] = ssid;
  target[kManualLatKey] = lat;
  target[kManualLonKey] = lon;
  target[kManualTzKey] = tz;
  target[kHasOffsetKey] = hasOffset;
  if (hasOffset) {
    target[kManualOffsetKey] = offsetMinutes;
  } else {
    target[kManualOffsetKey] = kOffsetUnknown;
  }
  if (!label.isEmpty()) {
    target[kManualLabelKey] = label;
  } else {
    target.remove(kManualLabelKey);
  }
  if (!city.isEmpty()) {
    target[kCityKey] = city;
  } else {
    target.remove(kCityKey);
  }

  platform::fs::File out = platform::fs::open(kManualSsidPath, FILE_WRITE);
  if (!out || out.isDirectory()) {
    return false;
  }
  const size_t written = serializeJson(doc, out);
  out.close();
  return written > 0;
}

bool GeoIpService::clearManualForSsid(const String& ssid) const {
  if (ssid.isEmpty()) {
    return false;
  }
  if (!platform::fs::exists(kManualSsidPath)) {
    return false;
  }

  platform::fs::File in = platform::fs::open(kManualSsidPath, FILE_READ);
  if (!in || in.isDirectory()) {
    if (in) {
      in.close();
    }
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, in);
  in.close();
  if (err) {
    return false;
  }

  JsonArray entries = doc[kEntriesKey].as<JsonArray>();
  if (entries.isNull()) {
    return false;
  }

  bool removed = false;
  for (int i = static_cast<int>(entries.size()) - 1; i >= 0; --i) {
    JsonObject entry = entries[i].as<JsonObject>();
    const String entrySsid = entry[kSsidKey] | String();
    if (entrySsid == ssid) {
      entries.remove(i);
      removed = true;
    }
  }

  if (!removed) {
    return false;
  }

  if (entries.size() == 0) {
    return platform::fs::remove(kManualSsidPath);
  }

  platform::fs::File out = platform::fs::open(kManualSsidPath, FILE_WRITE);
  if (!out || out.isDirectory()) {
    return false;
  }
  const size_t written = serializeJson(doc, out);
  out.close();
  return written > 0;
}

bool GeoIpService::parseOffsetText(const String& raw, int& minutes) const {
  String s = raw;
  s.trim();
  if (s.isEmpty()) {
    return false;
  }

  char sign = '+';
  if (s[0] == '+' || s[0] == '-') {
    sign = s[0];
    s = s.substring(1);
  }

  int hh = 0;
  int mm = 0;
  if (s.length() == 5 && s[2] == ':') {
    hh = s.substring(0, 2).toInt();
    mm = s.substring(3, 5).toInt();
  } else if (s.length() == 4) {
    hh = s.substring(0, 2).toInt();
    mm = s.substring(2, 4).toInt();
  } else {
    return false;
  }

  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
    return false;
  }

  minutes = hh * 60 + mm;
  if (sign == '-') {
    minutes = -minutes;
  }
  return true;
}

bool GeoIpService::fetchGeoForName(const String& name, float& lat, float& lon, String& tz,
                                   int& offsetMinutes, bool& hasOffset,
                                   String& label, String* errorOut) const {
  if (name.isEmpty()) {
    if (errorOut != nullptr) {
      *errorOut = "empty name";
    }
    return false;
  }

  const String urlOpenMeteo = "https://geocoding-api.open-meteo.com/v1/search?name=" +
                              urlEncode(name) + "&count=1&language=en&format=json";
  JsonDocument doc;
  String error;
  if (http_.get(urlOpenMeteo, doc, &error)) {
    JsonArrayConst results = doc["results"].as<JsonArrayConst>();
    if (!results.isNull() && results.size() > 0) {
      JsonObjectConst first = results[0].as<JsonObjectConst>();
      if (!first.isNull()) {
        lat = first["latitude"] | NAN;
        lon = first["longitude"] | NAN;
        tz = first["timezone"].as<String>();
        label = first["name"].as<String>();
        const String admin1 = first["admin1"].as<String>();
        const String country = first["country"].as<String>();
        if (!admin1.isEmpty()) {
          label += ", " + admin1;
        }
        if (!country.isEmpty()) {
          label += ", " + country;
        }

        hasOffset = false;
        offsetMinutes = 0;
        if (!tz.isEmpty()) {
          if (fetchOffsetForTimezone(tz, offsetMinutes)) {
            hasOffset = true;
          }
        }
        if (!isnan(lat) && !isnan(lon) && !tz.isEmpty()) {
          return true;
        }
      }
    } else {
      error = "open-meteo: no results";
    }
  } else {
    error = "open-meteo: " + error;
  }

  // Fallback geocoder for resilience.
  const String urlNominatim = "https://nominatim.openstreetmap.org/search?format=jsonv2&limit=1&q=" +
                              urlEncode(name);
  JsonDocument nomDoc;
  String nomError;
  if (!http_.get(urlNominatim, nomDoc, &nomError)) {
    if (errorOut != nullptr) {
      *errorOut = error + "; nominatim: " + nomError;
    }
    return false;
  }

  JsonArrayConst nomResults = nomDoc.as<JsonArrayConst>();
  if (nomResults.isNull() || nomResults.size() == 0) {
    if (errorOut != nullptr) {
      *errorOut = error + "; nominatim: no results";
    }
    return false;
  }

  JsonObjectConst firstNom = nomResults[0].as<JsonObjectConst>();
  if (firstNom.isNull()) {
    if (errorOut != nullptr) {
      *errorOut = error + "; nominatim: invalid result";
    }
    return false;
  }

  const String latText = firstNom["lat"].as<String>();
  const String lonText = firstNom["lon"].as<String>();
  lat = latText.toFloat();
  lon = lonText.toFloat();
  label = firstNom["display_name"].as<String>();
  tz = "";
  hasOffset = false;
  offsetMinutes = 0;
  if (isnan(lat) || isnan(lon) || (lat == 0.0f && lon == 0.0f && latText != "0" && lonText != "0")) {
    if (errorOut != nullptr) {
      *errorOut = error + "; nominatim: invalid coordinates";
    }
    return false;
  }

  if (!fetchTimezoneForLatLon(lat, lon, tz, offsetMinutes, hasOffset)) {
    if (errorOut != nullptr) {
      *errorOut = error + "; nominatim ok, timezone lookup failed";
    }
    return false;
  }

  if (errorOut != nullptr) {
    *errorOut = "";
  }
  return !tz.isEmpty();
}

bool GeoIpService::fetchTimezoneForLatLon(float lat, float lon, String& tz,
                                          int& offsetMinutes, bool& hasOffset) const {
  JsonDocument doc;
  String error;
  const String url =
      "https://api.open-meteo.com/v1/forecast?latitude=" + String(lat, 4) +
      "&longitude=" + String(lon, 4) +
      "&current=temperature_2m&timezone=auto";
  if (!http_.get(url, doc, &error)) {
    return false;
  }

  tz = doc["timezone"].as<String>();
  hasOffset = false;
  offsetMinutes = 0;
  if (!doc["utc_offset_seconds"].isNull()) {
    const int offsetSec = doc["utc_offset_seconds"].as<int>();
    offsetMinutes = offsetSec / 60;
    hasOffset = true;
  }
  if (!hasOffset && !tz.isEmpty()) {
    if (fetchOffsetForTimezone(tz, offsetMinutes)) {
      hasOffset = true;
    }
  }
  return !tz.isEmpty();
}

bool GeoIpService::setManualCity(const String& name) {
  float lat = NAN;
  float lon = NAN;
  String tz;
  int offsetMin = 0;
  bool hasOffset = false;
  String label;

  String geocodeError;
  if (!fetchGeoForName(name, lat, lon, tz, offsetMin, hasOffset, label, &geocodeError)) {
    setError("geocode failed: " + geocodeError);
    return false;
  }

  RuntimeGeo::setLocation(lat, lon, tz, offsetMin, hasOffset, label);
  saveManual(lat, lon, tz, offsetMin, hasOffset, label, name);
  lastSource_ = "manual";
  setError("");
  return true;
}

bool GeoIpService::setManualLatLon(float lat, float lon) {
  String tz;
  int offsetMin = 0;
  bool hasOffset = false;
  if (!fetchTimezoneForLatLon(lat, lon, tz, offsetMin, hasOffset)) {
    setError("timezone lookup failed");
    return false;
  }

  RuntimeGeo::setLocation(lat, lon, tz, offsetMin, hasOffset, String(lat, 4) + "," + String(lon, 4));
  saveManual(lat, lon, tz, offsetMin, hasOffset, "", "");
  lastSource_ = "manual";
  setError("");
  return true;
}

bool GeoIpService::fetchOffsetForTimezone(const String& tz, int& minutes) const {
  if (tz.isEmpty()) {
    return false;
  }

  JsonDocument doc;
  String error;
  const String url = "https://worldtimeapi.org/api/timezone/" + tz;
  if (!http_.get(url, doc, &error)) {
    return false;
  }

  if (!doc["utc_offset"].isNull()) {
    return parseOffsetText(doc["utc_offset"].as<String>(), minutes);
  }

  if (!doc["raw_offset"].isNull()) {
    const int raw = doc["raw_offset"].as<int>();
    const int dst = doc["dst_offset"] | 0;
    minutes = (raw + dst) / 60;
    return true;
  }

  return false;
}

bool GeoIpService::parseGeoDoc(const JsonDocument& doc, float& lat, float& lon,
                               String& tz, int& offsetMinutes,
                               bool& hasOffset) const {
  hasOffset = false;
  offsetMinutes = 0;

  if (!doc["latitude"].isNull() && !doc["longitude"].isNull()) {
    lat = doc["latitude"].as<float>();
    lon = doc["longitude"].as<float>();

    if (doc["timezone"].is<JsonObjectConst>()) {
      tz = doc["timezone"]["id"].as<String>();
      if (!doc["timezone"]["offset"].isNull()) {
        offsetMinutes = doc["timezone"]["offset"].as<int>() / 60;
        hasOffset = true;
      } else if (!doc["timezone"]["utc"].isNull()) {
        hasOffset = parseOffsetText(doc["timezone"]["utc"].as<String>(), offsetMinutes);
      }
    } else {
      tz = doc["timezone"].as<String>();
    }
  } else if (!doc["lat"].isNull() && !doc["lon"].isNull()) {
    lat = doc["lat"].as<float>();
    lon = doc["lon"].as<float>();
    tz = doc["timezone"].as<String>();
    if (!doc["utc_offset"].isNull()) {
      hasOffset = parseOffsetText(doc["utc_offset"].as<String>(), offsetMinutes);
    }
  } else if (!doc["loc"].isNull()) {
    const String loc = doc["loc"].as<String>();
    const int comma = loc.indexOf(',');
    if (comma > 0) {
      lat = loc.substring(0, comma).toFloat();
      lon = loc.substring(comma + 1).toFloat();
      tz = doc["timezone"].as<String>();
    } else {
      return false;
    }
  } else if (!doc["query"].isNull() && !doc["lat"].isNull() && !doc["lon"].isNull()) {
    lat = doc["lat"].as<float>();
    lon = doc["lon"].as<float>();
    tz = doc["timezone"].as<String>();
    if (!doc["offset"].isNull()) {
      offsetMinutes = doc["offset"].as<int>() / 60;
      hasOffset = true;
    }
  } else {
    return false;
  }

  return !isnan(lat) && !isnan(lon) && !tz.isEmpty();
}

String extractLabel(const JsonDocument& doc) {
  if (!doc["city"].isNull()) {
    String label = doc["city"].as<String>();
    String region = doc["region"].as<String>();
    if (region.isEmpty()) {
      region = doc["regionName"].as<String>();
    }
    String country = doc["country"].as<String>();
    if (country.isEmpty()) {
      country = doc["country_name"].as<String>();
    }
    if (!region.isEmpty()) {
      label += ", " + region;
    }
    if (!country.isEmpty()) {
      label += ", " + country;
    }
    return label;
  }
  return String();
}

bool GeoIpService::refreshFromInternet() {
  JsonDocument doc;
  String errPrimary;
  String errFallback;
  String errFallback2;
  String errFallback3;

  if (!http_.get(kGeoUrlPrimary, doc, &errPrimary)) {
    if (!http_.get(kGeoUrlFallback, doc, &errFallback)) {
      if (!http_.get(kGeoUrlFallback2, doc, &errFallback2)) {
        if (!http_.get(kGeoUrlFallback3, doc, &errFallback3)) {
          lastSource_ = "none";
          setError("primary=" + errPrimary + ", fallback1=" + errFallback +
                   ", fallback2=" + errFallback2 + ", fallback3=" + errFallback3);
          return false;
        }
        lastSource_ = kGeoUrlFallback3;
      } else {
        lastSource_ = kGeoUrlFallback2;
      }
    } else {
      lastSource_ = kGeoUrlFallback;
    }
  } else {
    lastSource_ = kGeoUrlPrimary;
  }

  float lat = NAN;
  float lon = NAN;
  String tz;
  int offsetMin = 0;
  bool hasOffset = false;
  if (!parseGeoDoc(doc, lat, lon, tz, offsetMin, hasOffset)) {
    setError("geo response missing latitude/longitude/timezone");
    return false;
  }

  if (!hasOffset && !tz.isEmpty()) {
    int resolvedOffset = 0;
    if (fetchOffsetForTimezone(tz, resolvedOffset)) {
      offsetMin = resolvedOffset;
      hasOffset = true;
      platform::logi("geo", "timezone offset resolved from worldtimeapi tz=%s off_min=%d",
                    tz.c_str(), offsetMin);
    } else {
      platform::logw("geo", "timezone offset unresolved tz=%s", tz.c_str());
    }
  }

  const String label = extractLabel(doc);
  RuntimeGeo::setLocation(lat, lon, tz, offsetMin, hasOffset, label);
  saveCached(lat, lon, tz, label);
  setError("");
  return true;
}
