#include "services/GeoIpService.h"

#include <ArduinoJson.h>

#include "RuntimeGeo.h"

namespace {
constexpr char kPrefsNs[] = "geo";
constexpr char kLatKey[] = "lat";
constexpr char kLonKey[] = "lon";
constexpr char kTzKey[] = "tz";
constexpr char kOffsetKey[] = "off_min";
constexpr int kOffsetUnknown = -32768;

constexpr char kGeoUrlPrimary[] = "https://ipwho.is/";
constexpr char kGeoUrlFallback[] = "https://ipapi.co/json/";
constexpr char kGeoUrlFallback2[] = "https://ipinfo.io/json";
constexpr char kGeoUrlFallback3[] = "http://ip-api.com/json/";
}  // namespace

void GeoIpService::setError(const String& msg) {
  lastError_ = msg;
}

bool GeoIpService::loadCached() {
  prefs_.begin(kPrefsNs, true);
  const float lat = prefs_.getFloat(kLatKey, NAN);
  const float lon = prefs_.getFloat(kLonKey, NAN);
  const String tz = prefs_.getString(kTzKey, "");
  const int offMin = prefs_.getInt(kOffsetKey, kOffsetUnknown);
  prefs_.end();

  if (isnan(lat) || isnan(lon) || tz.isEmpty()) {
    setError("cache missing lat/lon/tz");
    return false;
  }

  const bool hasOffset = offMin != kOffsetUnknown;
  RuntimeGeo::setLocation(lat, lon, tz, hasOffset ? offMin : 0, hasOffset);
  lastSource_ = "nvs-cache";
  setError("");
  return true;
}

bool GeoIpService::saveCached(float lat, float lon, const String& tz) {
  prefs_.begin(kPrefsNs, false);
  prefs_.putFloat(kLatKey, lat);
  prefs_.putFloat(kLonKey, lon);
  prefs_.putString(kTzKey, tz);
  if (RuntimeGeo::hasUtcOffset) {
    prefs_.putInt(kOffsetKey, RuntimeGeo::utcOffsetMinutes);
  }
  prefs_.end();
  return true;
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
      Serial.printf("[geo] timezone offset resolved from worldtimeapi tz=%s off_min=%d\n",
                    tz.c_str(), offsetMin);
    } else {
      Serial.printf("[geo] timezone offset unresolved tz=%s\n", tz.c_str());
    }
  }

  RuntimeGeo::setLocation(lat, lon, tz, offsetMin, hasOffset);
  saveCached(lat, lon, tz);
  setError("");
  return true;
}
