#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "services/HttpJsonClient.h"

class GeoIpService {
 public:
  bool loadOverride();
  bool loadCached();
  bool refreshFromInternet();
  bool setManualCity(const String& name);
  bool setManualLatLon(float lat, float lon);
  bool clearOverride();
  String lastError() const { return lastError_; }
  String lastSource() const { return lastSource_; }

 private:
  bool fetchGeoForName(const String& name, float& lat, float& lon, String& tz,
                       int& offsetMinutes, bool& hasOffset, String& label) const;
  bool fetchTimezoneForLatLon(float lat, float lon, String& tz, int& offsetMinutes,
                              bool& hasOffset) const;
  bool saveManual(float lat, float lon, const String& tz, int offsetMinutes,
                  bool hasOffset, const String& label, const String& city);
  bool loadManualForSsid(const String& ssid, float& lat, float& lon, String& tz,
                         int& offsetMinutes, bool& hasOffset, String& label,
                         String& city) const;
  bool saveManualForSsid(const String& ssid, float lat, float lon, const String& tz,
                         int offsetMinutes, bool hasOffset, const String& label,
                         const String& city) const;
  bool clearManualForSsid(const String& ssid) const;
  String currentWifiSsid() const;
  bool parseGeoDoc(const JsonDocument& doc, float& lat, float& lon, String& tz,
                   int& offsetMinutes, bool& hasOffset) const;
  bool parseOffsetText(const String& raw, int& minutes) const;
  bool fetchOffsetForTimezone(const String& tz, int& minutes) const;
  bool saveCached(float lat, float lon, const String& tz, const String& label);
  void setError(const String& msg);

  HttpJsonClient http_;
  Preferences prefs_;
  String lastError_;
  String lastSource_;
};
