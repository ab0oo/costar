// DslTime.cpp — geo context, timestamp parsing, time/numeric formatting.

#include "DslTime.h"

#include "AppConfig.h"
#include "DslJson.h"
#include "RuntimeSettings.h"
#include "platform/Prefs.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

using dsl_json::replaceAll;

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

std::string readFile(const char* path) {
  if (path == nullptr || *path == '\0') {
    return {};
  }
  std::FILE* fp = std::fopen(path, "rb");
  if (fp == nullptr) {
    return {};
  }
  if (std::fseek(fp, 0, SEEK_END) != 0) {
    std::fclose(fp);
    return {};
  }
  const long len = std::ftell(fp);
  if (len <= 0) {
    std::fclose(fp);
    return {};
  }
  if (std::fseek(fp, 0, SEEK_SET) != 0) {
    std::fclose(fp);
    return {};
  }
  std::string out(static_cast<size_t>(len), '\0');
  const size_t got = std::fread(out.data(), 1, out.size(), fp);
  std::fclose(fp);
  if (got != out.size()) {
    return {};
  }
  return out;
}

// ---------------------------------------------------------------------------
// Geo context
// ---------------------------------------------------------------------------

float loadGeoLat() {
  const int mode = static_cast<int>(platform::prefs::getInt("geo", "mode", 0));
  if (mode == 1) {
    const float manual = platform::prefs::getFloat("geo", "mlat", NAN);
    if (!std::isnan(manual)) {
      return manual;
    }
  }
  const float cached = platform::prefs::getFloat("geo", "lat", NAN);
  return std::isnan(cached) ? AppConfig::kDefaultLatitude : cached;
}

float loadGeoLon() {
  const int mode = static_cast<int>(platform::prefs::getInt("geo", "mode", 0));
  if (mode == 1) {
    const float manual = platform::prefs::getFloat("geo", "mlon", NAN);
    if (!std::isnan(manual)) {
      return manual;
    }
  }
  const float cached = platform::prefs::getFloat("geo", "lon", NAN);
  return std::isnan(cached) ? AppConfig::kDefaultLongitude : cached;
}

std::string loadGeoTimezone() {
  const int mode = static_cast<int>(platform::prefs::getInt("geo", "mode", 0));
  if (mode == 1) {
    const std::string manualTz = platform::prefs::getString("geo", "mtz", "");
    if (!manualTz.empty()) {
      return manualTz;
    }
  }
  return platform::prefs::getString("geo", "tz", "");
}

bool loadGeoOffsetMinutes(int& out) {
  constexpr int kOffsetUnknown = -32768;
  const int mode = static_cast<int>(platform::prefs::getInt("geo", "mode", 0));
  if (mode == 1) {
    const int manual = static_cast<int>(platform::prefs::getInt("geo", "moff", kOffsetUnknown));
    if (manual != kOffsetUnknown) {
      out = manual;
      return true;
    }
  }
  const int cached = static_cast<int>(platform::prefs::getInt("geo", "off_min", kOffsetUnknown));
  if (cached != kOffsetUnknown) {
    out = cached;
    return true;
  }
  return false;
}

bool inferOffsetFromTimezone(const std::string& tz, int& outMinutes) {
  if (tz == "America/Los_Angeles") {
    outMinutes = -8 * 60;
    return true;
  }
  if (tz == "America/Denver") {
    outMinutes = -7 * 60;
    return true;
  }
  if (tz == "America/Chicago") {
    outMinutes = -6 * 60;
    return true;
  }
  if (tz == "America/New_York") {
    outMinutes = -5 * 60;
    return true;
  }
  if (tz == "UTC" || tz == "Etc/UTC") {
    outMinutes = 0;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Timestamp parsing
// ---------------------------------------------------------------------------

bool parseTzOffsetMinutes(const std::string& tz, int& minutes) {
  if (tz.size() < 9) {
    return false;
  }
  if (tz.rfind("UTC", 0) != 0) {
    return false;
  }
  const char sign = tz[3];
  if ((sign != '+' && sign != '-') || tz[6] != ':') {
    return false;
  }

  const std::string hhText = tz.substr(4, 2);
  const std::string mmText = tz.substr(7, 2);
  double hh = 0.0;
  double mm = 0.0;
  if (!dsl_json::parseStrictDouble(hhText, hh) || !dsl_json::parseStrictDouble(mmText, mm)) {
    return false;
  }
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
    return false;
  }

  minutes = static_cast<int>(hh) * 60 + static_cast<int>(mm);
  if (sign == '-') {
    minutes = -minutes;
  }
  return true;
}

bool parseIsoMinuteTimestamp(const std::string& text, int& year, int& mon, int& day, int& hour,
                             int& minute) {
  if (text.size() < 10) {
    return false;
  }

  const std::string y = text.substr(0, 4);
  const std::string mo = text.substr(5, 2);
  const std::string d = text.substr(8, 2);
  double yv = 0.0;
  double mov = 0.0;
  double dv = 0.0;
  if (!dsl_json::parseStrictDouble(y, yv) || !dsl_json::parseStrictDouble(mo, mov) ||
      !dsl_json::parseStrictDouble(d, dv)) {
    return false;
  }

  year = static_cast<int>(yv);
  mon = static_cast<int>(mov);
  day = static_cast<int>(dv);
  hour = 0;
  minute = 0;

  if (text.size() >= 16) {
    const std::string hh = text.substr(11, 2);
    const std::string mm = text.substr(14, 2);
    double hhv = 0.0;
    double mmv = 0.0;
    if (!dsl_json::parseStrictDouble(hh, hhv) || !dsl_json::parseStrictDouble(mm, mmv)) {
      return false;
    }
    hour = static_cast<int>(hhv);
    minute = static_cast<int>(mmv);
  }

  if (year < 1970 || mon < 1 || mon > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 ||
      minute < 0 || minute > 59) {
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Calendar math
// ---------------------------------------------------------------------------

long long daysFromCivil(int year, int mon, int day) {
  year -= mon <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy =
      (153 * static_cast<unsigned>(mon + (mon > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(day) - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<long long>(era) * 146097LL + static_cast<long long>(doe) - 719468LL;
}

void civilFromDays(long long z, int& year, int& mon, int& day) {
  z += 719468;
  const long long era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  year = static_cast<int>(yoe) + static_cast<int>(era) * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  mon = static_cast<int>(mp + (mp < 10 ? 3 : -9));
  year += (mon <= 2);
}

// ---------------------------------------------------------------------------
// Timestamp formatting
// ---------------------------------------------------------------------------

std::string formatTimestampWithTz(const std::string& text, const std::string& tz,
                                  const std::string& timeFormat) {
  int offsetMinutes = 0;
  std::string tzSource = tz;

  if (tzSource == "local") {
    if (!loadGeoOffsetMinutes(offsetMinutes)) {
      const std::string geoTz = loadGeoTimezone();
      if (!inferOffsetFromTimezone(geoTz, offsetMinutes)) {
        offsetMinutes = 0;
      }
    }
    char tzBuf[16];
    const char sign = (offsetMinutes < 0) ? '-' : '+';
    const int absMin = std::abs(offsetMinutes);
    std::snprintf(tzBuf, sizeof(tzBuf), "UTC%c%02d:%02d", sign, absMin / 60, absMin % 60);
    tzSource = tzBuf;
  }

  if (!parseTzOffsetMinutes(tzSource, offsetMinutes)) {
    return text;
  }

  int year = 0;
  int mon = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  if (!parseIsoMinuteTimestamp(text, year, mon, day, hour, minute)) {
    return text;
  }

  long long totalMinutes = daysFromCivil(year, mon, day) * 1440LL +
                           static_cast<long long>(hour) * 60LL + static_cast<long long>(minute);
  totalMinutes += static_cast<long long>(offsetMinutes);

  long long dayCount = totalMinutes / 1440LL;
  int rem = static_cast<int>(totalMinutes % 1440LL);
  if (rem < 0) {
    rem += 1440;
    --dayCount;
  }

  int outYear = 0;
  int outMon = 0;
  int outDay = 0;
  civilFromDays(dayCount, outYear, outMon, outDay);

  const int outHour = rem / 60;
  const int outMinute = rem % 60;

  int dow = static_cast<int>((dayCount + 4) % 7);
  if (dow < 0) {
    dow += 7;
  }

  static const char* kDowShort[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char* kDowLong[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                   "Thursday", "Friday", "Saturday"};
  static const char* kMonthShort[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  static const char* kMonthLong[] = {"January", "February", "March", "April", "May", "June",
                                     "July", "August", "September", "October", "November",
                                     "December"};

  auto isoWeekNumber = [&](int y, int m, int d) {
    const long long dayNum = daysFromCivil(y, m, d);
    int dowMon = static_cast<int>((dayNum + 3) % 7);
    if (dowMon < 0) {
      dowMon += 7;
    }
    dowMon += 1;

    const long long jan1 = daysFromCivil(y, 1, 1);
    int jan1Dow = static_cast<int>((jan1 + 3) % 7);
    if (jan1Dow < 0) {
      jan1Dow += 7;
    }
    jan1Dow += 1;

    const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    const bool has53 = (jan1Dow == 4) || (leap && jan1Dow == 3);

    const int doy = static_cast<int>(dayNum - jan1) + 1;
    int week = (doy - dowMon + 10) / 7;
    if (week < 1) {
      const int prevYear = y - 1;
      const long long prevJan1 = daysFromCivil(prevYear, 1, 1);
      int prevJan1Dow = static_cast<int>((prevJan1 + 3) % 7);
      if (prevJan1Dow < 0) {
        prevJan1Dow += 7;
      }
      prevJan1Dow += 1;
      const bool prevLeap =
          (prevYear % 4 == 0 && prevYear % 100 != 0) || (prevYear % 400 == 0);
      const bool prevHas53 = (prevJan1Dow == 4) || (prevLeap && prevJan1Dow == 3);
      week = prevHas53 ? 53 : 52;
    } else if (week == 53 && !has53) {
      week = 1;
    }
    return week;
  };

  std::string out = timeFormat.empty() ? "%H:%M" : timeFormat;

  auto replaceToken = [&](const std::string& token, const std::string& value) {
    out = replaceAll(out, token, value);
  };

  char num[16];
  std::snprintf(num, sizeof(num), "%04d", outYear);
  replaceToken("%Y", num);
  std::snprintf(num, sizeof(num), "%02d", outMon);
  replaceToken("%m", num);
  std::snprintf(num, sizeof(num), "%02d", outDay);
  replaceToken("%d", num);
  std::snprintf(num, sizeof(num), "%02d", outHour);
  replaceToken("%H", num);
  std::snprintf(num, sizeof(num), "%02d", outMinute);
  replaceToken("%M", num);

  replaceToken("%a", kDowShort[dow]);
  replaceToken("%A", kDowLong[dow]);
  if (outMon >= 1 && outMon <= 12) {
    replaceToken("%b", kMonthShort[outMon - 1]);
    replaceToken("%B", kMonthLong[outMon - 1]);
  }

  std::snprintf(num, sizeof(num), "%02d", isoWeekNumber(outYear, outMon, outDay));
  replaceToken("%V", num);

  return out;
}

// ---------------------------------------------------------------------------
// Numeric and compass formatting
// ---------------------------------------------------------------------------

std::string formatNumericLocale(double value, int decimals, const std::string& locale) {
  if (decimals < 0) {
    decimals = 0;
  }
  if (decimals > 6) {
    decimals = 6;
  }

  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
  std::string text = buf;

  const size_t dotPos = text.find('.');
  std::string intPart = (dotPos == std::string::npos) ? text : text.substr(0, dotPos);
  std::string fracPart = (dotPos == std::string::npos) ? std::string() : text.substr(dotPos + 1);

  bool negative = false;
  if (!intPart.empty() && intPart.front() == '-') {
    negative = true;
    intPart.erase(intPart.begin());
  }

  const bool euroStyle = (locale == "de-DE" || locale == "fr-FR" || locale == "es-ES");
  const char thousandsSep = euroStyle ? '.' : ',';
  const char decimalSep = euroStyle ? ',' : '.';

  std::string grouped;
  grouped.reserve(intPart.size() + intPart.size() / 3 + 2);
  for (size_t i = 0; i < intPart.size(); ++i) {
    grouped.push_back(intPart[i]);
    const size_t rem = intPart.size() - i - 1;
    if (rem > 0 && rem % 3 == 0) {
      grouped.push_back(thousandsSep);
    }
  }

  std::string out = negative ? ("-" + grouped) : grouped;
  if (decimals > 0) {
    out.push_back(decimalSep);
    out += fracPart;
  }
  return out;
}

std::string formatCompass16(double deg) {
  static constexpr const char* kDirs[16] = {
      "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
      "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW",
  };
  if (!std::isfinite(deg)) {
    return "";
  }
  double normalized = std::fmod(deg, 360.0);
  if (normalized < 0.0) {
    normalized += 360.0;
  }
  const int idx = static_cast<int>(std::floor((normalized + 11.25) / 22.5)) & 15;
  return kDirs[idx];
}

std::string applyFormat(const std::string& rawText, const FormatSpec& fmt, bool numeric,
                        double numericValue) {
  std::string out = numeric ? std::string() : rawText;
  if (!fmt.valueMap.empty()) {
    std::string key = rawText;
    size_t start = 0;
    while (start < key.size() && std::isspace(static_cast<unsigned char>(key[start]))) {
      ++start;
    }
    size_t end = key.size();
    while (end > start && std::isspace(static_cast<unsigned char>(key[end - 1]))) {
      --end;
    }
    key = key.substr(start, end - start);
    auto it = fmt.valueMap.find(key);
    if (it == fmt.valueMap.end() && numeric && std::isfinite(numericValue)) {
      const double rounded = std::round(numericValue);
      if (std::fabs(numericValue - rounded) < 0.000001) {
        key = std::to_string(static_cast<long long>(rounded));
        it = fmt.valueMap.find(key);
      }
    }
    if (it != fmt.valueMap.end()) {
      out = it->second;
      numeric = false;
    } else if (!fmt.valueMapDefault.empty()) {
      out = fmt.valueMapDefault;
      numeric = false;
    }
  }

  if (!fmt.tz.empty()) {
    out = formatTimestampWithTz(rawText, fmt.tz, fmt.timeFormat);
  }

  double value = numericValue;
  std::string unitSuffix;
  std::string unitLower = fmt.unit;
  std::transform(unitLower.begin(), unitLower.end(), unitLower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });

  if (numeric && !unitLower.empty()) {
    if (unitLower == "f" || unitLower == "fahrenheit" || unitLower == "c_to_f") {
      value = (value * 9.0 / 5.0) + 32.0;
      unitSuffix = " F";
    } else if (unitLower == "c" || unitLower == "celsius") {
      unitSuffix = " C";
    } else if (unitLower == "pressure") {
      if (RuntimeSettings::useFahrenheit) {
        value = value * 0.0295299830714;
        unitSuffix = " inHg";
      } else {
        unitSuffix = " hPa";
      }
    } else if (unitLower == "percent" || unitLower == "%") {
      unitSuffix = "%";
    } else if (unitLower == "compass_16" || unitLower == "wind_dir" || unitLower == "cardinal_16") {
      out = formatCompass16(value);
      numeric = false;
    }
  }

  if (numeric) {
    int decimals = 2;
    if (fmt.roundDigits >= 0) {
      decimals = fmt.roundDigits;
    } else if (unitLower == "pressure") {
      decimals = RuntimeSettings::useFahrenheit ? 2 : 0;
    }
    out += formatNumericLocale(value, decimals, fmt.locale);
  }

  if (!fmt.prefix.empty()) {
    out = fmt.prefix + out;
  }
  if (!fmt.suffix.empty()) {
    out += fmt.suffix;
  } else if (!unitSuffix.empty()) {
    out += unitSuffix;
  }

  return out;
}
