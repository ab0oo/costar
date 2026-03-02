#pragma once

#include <cmath>
#include <string>

struct GeoContext {
  float lat = NAN;
  float lon = NAN;
  std::string timezone;
  int utcOffsetMinutes = 0;
  bool hasUtcOffset = false;
  std::string source = "none";
  bool hasLocation = false;
};

bool refreshGeoContextFromInternet(GeoContext& geoOut);
bool isGeoValid(float lat, float lon, const std::string& timezone);
