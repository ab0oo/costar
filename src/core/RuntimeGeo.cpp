#include "RuntimeGeo.h"

#include "AppConfig.h"

namespace RuntimeGeo {
float latitude = AppConfig::kDefaultLatitude;
float longitude = AppConfig::kDefaultLongitude;
String timezone;
bool hasLocation = false;
int utcOffsetMinutes = 0;
bool hasUtcOffset = false;

void setLocation(float lat, float lon, const String& tz, int offsetMinutes,
                 bool offsetKnown) {
  latitude = lat;
  longitude = lon;
  timezone = tz;
  utcOffsetMinutes = offsetMinutes;
  hasUtcOffset = offsetKnown;
  hasLocation = true;
}
}  // namespace RuntimeGeo
