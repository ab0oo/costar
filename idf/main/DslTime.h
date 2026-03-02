#pragma once

#include <cstdint>
#include <string>

// Geo context helpers — read from NVS prefs, used by time formatting and transforms.
float loadGeoLat();
float loadGeoLon();
std::string loadGeoTimezone();
bool loadGeoOffsetMinutes(int& out);
bool inferOffsetFromTimezone(const std::string& tz, int& outMinutes);

// File I/O helper.
std::string readFile(const char* path);

// Time/timestamp functions.
bool parseTzOffsetMinutes(const std::string& tz, int& minutes);
bool parseIsoMinuteTimestamp(const std::string& text, int& year, int& mon, int& day,
                             int& hour, int& minute);
long long daysFromCivil(int year, int mon, int day);
void civilFromDays(long long z, int& year, int& mon, int& day);
std::string formatTimestampWithTz(const std::string& text, const std::string& tz,
                                  const std::string& timeFormat);

// Numeric and compass formatting.
std::string formatNumericLocale(double value, int decimals, const std::string& locale);
std::string formatCompass16(double deg);

// Format specification — used by applyFormat and field resolution.
struct FormatSpec {
  int roundDigits = -1;
  std::string unit;
  std::string locale;
  std::string prefix;
  std::string suffix;
  std::string tz;
  std::string timeFormat;
};

std::string applyFormat(const std::string& rawText, const FormatSpec& fmt, bool numeric,
                        double numericValue);
