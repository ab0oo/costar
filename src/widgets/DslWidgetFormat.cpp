#include "widgets/DslWidget.h"

#include <stdlib.h>

#include "RuntimeGeo.h"

String DslWidget::applyFormat(const String& text, const dsl::FormatSpec& fmt,
                              bool numeric, double numericValue) const {
  String out = numeric ? String() : text;

  if (!fmt.tz.isEmpty()) {
    out = formatTimestampWithTz(text, fmt.tz, fmt.timeFormat);
  }

  double value = numericValue;

  String unitSuffix;
  String unit = fmt.unit;
  unit.toLowerCase();
  if (numeric && !unit.isEmpty()) {
    if (unit == "f" || unit == "fahrenheit" || unit == "c_to_f") {
      value = (value * 9.0 / 5.0) + 32.0;
      unitSuffix = " F";
    } else if (unit == "c" || unit == "celsius") {
      unitSuffix = " C";
    } else if (unit == "percent" || unit == "%") {
      unitSuffix = "%";
    } else if (unit == "usd" || unit == "$") {
      unitSuffix = "";
      if (fmt.prefix.isEmpty()) {
        out = "$";
      }
    }
  }

  if (numeric && fmt.roundDigits >= 0) {
    out += formatNumericLocale(value, fmt.roundDigits, fmt.locale);
  } else if (numeric) {
    out += formatNumericLocale(value, 2, fmt.locale);
  }

  if (!fmt.prefix.isEmpty()) {
    out = fmt.prefix + out;
  }
  if (!fmt.suffix.isEmpty()) {
    out += fmt.suffix;
  }
  if (!unitSuffix.isEmpty() && fmt.suffix.isEmpty()) {
    out += unitSuffix;
  }

  return out;
}

String DslWidget::formatNumericLocale(double value, int decimals, const String& locale) const {
  char buf[48];
  snprintf(buf, sizeof(buf), "%.*f", decimals, value);
  String s = String(buf);

  int dotIdx = s.indexOf('.');
  String intPart = dotIdx >= 0 ? s.substring(0, dotIdx) : s;
  String fracPart = dotIdx >= 0 ? s.substring(dotIdx + 1) : String();

  bool negative = false;
  if (intPart.startsWith("-")) {
    negative = true;
    intPart = intPart.substring(1);
  }

  const bool euroStyle = locale == "de-DE" || locale == "fr-FR" || locale == "es-ES";
  const char thousandsSep = euroStyle ? '.' : ',';
  const char decimalSep = euroStyle ? ',' : '.';

  String grouped;
  for (int i = 0; i < intPart.length(); ++i) {
    grouped += intPart[i];
    const int rem = intPart.length() - i - 1;
    if (rem > 0 && rem % 3 == 0) {
      grouped += thousandsSep;
    }
  }

  String out = negative ? String("-") + grouped : grouped;
  if (decimals > 0) {
    out += decimalSep;
    out += fracPart;
  }
  return out;
}

bool DslWidget::parseTzOffsetMinutes(const String& tz, int& minutes) const {
  if (tz.length() < 9) {
    return false;
  }

  const char sign = tz[3];
  if ((sign != '+' && sign != '-') || tz[6] != ':') {
    return false;
  }

  const int hh = tz.substring(4, 6).toInt();
  const int mm = tz.substring(7, 9).toInt();
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
    return false;
  }

  minutes = hh * 60 + mm;
  if (sign == '-') {
    minutes = -minutes;
  }
  return true;
}

bool DslWidget::parseIsoMinuteTimestamp(const String& text, int& year, int& mon, int& day,
                                        int& hour, int& minute) const {
  if (text.length() < 16) {
    return false;
  }

  year = text.substring(0, 4).toInt();
  mon = text.substring(5, 7).toInt();
  day = text.substring(8, 10).toInt();
  hour = text.substring(11, 13).toInt();
  minute = text.substring(14, 16).toInt();

  if (year < 1970 || mon < 1 || mon > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 ||
      minute < 0 || minute > 59) {
    return false;
  }
  return true;
}

long long DslWidget::daysFromCivil(int year, int mon, int day) const {
  year -= mon <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(day) -
                       1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097LL + static_cast<long long>(doe) - 719468LL;
}

void DslWidget::civilFromDays(long long z, int& year, int& mon, int& day) const {
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

String DslWidget::formatTimestampWithTz(const String& text, const String& tz,
                                        const String& timeFormat) const {
  int tzOffsetMin = 0;
  String tzSource = tz;
  if (tz.equalsIgnoreCase("local")) {
    if (!RuntimeGeo::hasUtcOffset) {
      return text;
    }
    const int off = RuntimeGeo::utcOffsetMinutes;
    const char sign = off < 0 ? '-' : '+';
    const int absMin = abs(off);
    const int hh = absMin / 60;
    const int mm = absMin % 60;
    char tzBuf[16];
    snprintf(tzBuf, sizeof(tzBuf), "UTC%c%02d:%02d", sign, hh, mm);
    tzSource = String(tzBuf);
  }

  if (!parseTzOffsetMinutes(tzSource, tzOffsetMin)) {
    return text;
  }

  int y = 0;
  int mo = 0;
  int d = 0;
  int hh = 0;
  int mm = 0;
  if (!parseIsoMinuteTimestamp(text, y, mo, d, hh, mm)) {
    return text;
  }

  long long totalMinutes = daysFromCivil(y, mo, d) * 1440LL + hh * 60LL + mm;
  totalMinutes += tzOffsetMin;

  long long days = totalMinutes / 1440LL;
  int rem = static_cast<int>(totalMinutes % 1440LL);
  if (rem < 0) {
    rem += 1440;
    --days;
  }

  int outY = 0;
  int outMo = 0;
  int outD = 0;
  civilFromDays(days, outY, outMo, outD);

  const int outH = rem / 60;
  const int outM = rem % 60;

  String out = timeFormat;
  out.replace("%Y", String(outY));
  out.replace("%m", (outMo < 10 ? "0" : "") + String(outMo));
  out.replace("%d", (outD < 10 ? "0" : "") + String(outD));
  out.replace("%H", (outH < 10 ? "0" : "") + String(outH));
  out.replace("%M", (outM < 10 ? "0" : "") + String(outM));
  return out;
}
