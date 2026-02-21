#pragma once

#include <Arduino.h>

namespace RuntimeGeo {
extern float latitude;
extern float longitude;
extern String timezone;
extern String label;
extern bool hasLocation;
extern int utcOffsetMinutes;
extern bool hasUtcOffset;

void setLocation(float lat, float lon, const String& tz, int offsetMinutes = 0,
                 bool offsetKnown = false, const String& labelText = "");
}
