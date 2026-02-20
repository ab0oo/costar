#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "services/HttpJsonClient.h"

class GeoIpService {
 public:
  bool loadCached();
  bool refreshFromInternet();
  String lastError() const { return lastError_; }
  String lastSource() const { return lastSource_; }

 private:
  bool parseGeoDoc(const JsonDocument& doc, float& lat, float& lon, String& tz,
                   int& offsetMinutes, bool& hasOffset) const;
  bool parseOffsetText(const String& raw, int& minutes) const;
  bool fetchOffsetForTimezone(const String& tz, int& minutes) const;
  bool saveCached(float lat, float lon, const String& tz);
  void setError(const String& msg);

  HttpJsonClient http_;
  Preferences prefs_;
  String lastError_;
  String lastSource_;
};
