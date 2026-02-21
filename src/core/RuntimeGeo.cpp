#include "RuntimeGeo.h"

#include "AppConfig.h"

namespace RuntimeGeo {
float latitude = AppConfig::kDefaultLatitude;
float longitude = AppConfig::kDefaultLongitude;
String timezone;
String label;
bool hasLocation = false;
int utcOffsetMinutes = 0;
bool hasUtcOffset = false;

void setLocation(float lat, float lon, const String& tz, int offsetMinutes,
                 bool offsetKnown, const String& labelText) {
  latitude = lat;
  longitude = lon;
  timezone = tz;
  label = labelText;
  utcOffsetMinutes = offsetMinutes;
  hasUtcOffset = offsetKnown;
  hasLocation = true;
}
}  // namespace RuntimeGeo
