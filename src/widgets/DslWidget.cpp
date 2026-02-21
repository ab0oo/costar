#include "widgets/DslWidget.h"

#include <algorithm>

#include "RuntimeGeo.h"
#include "RuntimeSettings.h"
#include "dsl/DslParser.h"

#include <math.h>

namespace {
void mapWeatherCode(int code, String& outText, String& outIcon) {
  outText = "Unknown";
  outIcon = "/icons/meteocons/cloudy.raw";

  if (code == 0) {
    outText = "Clear";
    outIcon = "/icons/meteocons/clear-day.raw";
  } else if (code == 1) {
    outText = "Mostly Clear";
    outIcon = "/icons/meteocons/partly-cloudy-day.raw";
  } else if (code == 2) {
    outText = "Partly Cloudy";
    outIcon = "/icons/meteocons/partly-cloudy-day.raw";
  } else if (code == 3) {
    outText = "Overcast";
    outIcon = "/icons/meteocons/cloudy.raw";
  } else if (code == 45 || code == 48) {
    outText = "Fog";
    outIcon = "/icons/meteocons/fog.raw";
  } else if (code == 51 || code == 53 || code == 55 || code == 56 || code == 57) {
    outText = "Drizzle";
    outIcon = "/icons/meteocons/drizzle.raw";
  } else if (code == 61 || code == 63 || code == 65 || code == 66 || code == 67 ||
             code == 80 || code == 81 || code == 82) {
    outText = "Rain";
    outIcon = "/icons/meteocons/rain.raw";
  } else if (code == 71 || code == 73 || code == 75 || code == 77 || code == 85 ||
             code == 86) {
    outText = "Snow";
    outIcon = "/icons/meteocons/snow.raw";
  } else if (code == 95 || code == 96 || code == 99) {
    outText = "Storm";
    outIcon = "/icons/meteocons/thunderstorms-day.raw";
  }
}
}  // namespace

DslWidget::DslWidget(const WidgetConfig& cfg) : Widget(cfg) {
  auto pathIt = config_.settings.find("dsl_path");
  if (pathIt != config_.settings.end()) {
    dslPath_ = pathIt->second;
  }

  auto spriteIt = config_.settings.find("use_sprite");
  if (spriteIt != config_.settings.end()) {
    const String value = spriteIt->second;
    if (value.equalsIgnoreCase("true") || value == "1" || value.equalsIgnoreCase("yes") ||
        value.equalsIgnoreCase("on")) {
      useSprite_ = true;
    }
  }

  auto debugIt = config_.settings.find("debug");
  if (debugIt != config_.settings.end()) {
    const String value = debugIt->second;
    if (value.equalsIgnoreCase("true") || value == "1" || value.equalsIgnoreCase("yes") ||
        value.equalsIgnoreCase("on")) {
      debugOverride_ = true;
    }
  }
}

DslWidget::~DslWidget() {
  if (sprite_ != nullptr) {
    sprite_->deleteSprite();
    delete sprite_;
    sprite_ = nullptr;
  }
}
void DslWidget::begin() {
  Widget::begin();
  dslLoaded_ = loadDslModel();
  if (dslLoaded_) {
    // Force first DSL fetch immediately; do not wait full poll interval.
    const uint32_t nowMs = millis();
    lastFetchMs_ = (nowMs > dsl_.pollMs) ? (nowMs - dsl_.pollMs) : 0;
    nextFetchMs_ = nowMs;
    firstFetch_ = true;
  }
}

bool DslWidget::loadDslModel() {
  if (dslPath_.isEmpty()) {
    status_ = "missing dsl_path";
    return false;
  }

  String parseErr;
  dsl::Document parsed;
  if (!dsl::Parser::parseFile(dslPath_, parsed, &parseErr)) {
    status_ = parseErr;
    return false;
  }

  dsl_ = parsed;
  if (debugOverride_) {
    dsl_.debug = true;
  }
  status_ = "dsl ok";
  return true;
}

String DslWidget::bindRuntimeTemplate(const String& input) const {
  String out = input;
  int start = out.indexOf("{{");

  while (start >= 0) {
    const int end = out.indexOf("}}", start + 2);
    if (end < 0) {
      break;
    }

    const String key = out.substring(start + 2, end);
    String value;

    if (key == "geo.lat") {
      value = String(RuntimeGeo::latitude, 4);
    } else if (key == "geo.lon") {
      value = String(RuntimeGeo::longitude, 4);
    } else if (key == "geo.tz") {
      value = RuntimeGeo::timezone;
    } else if (key == "geo.label") {
      value = RuntimeGeo::label;
    } else if (key == "geo.offset_min") {
      value = String(RuntimeGeo::utcOffsetMinutes);
    } else if (key.startsWith("setting.")) {
      const String settingKey = key.substring(8);
      if (settingKey == "radius_nm" && RuntimeSettings::adsbRadiusNm > 0) {
        value = String(RuntimeSettings::adsbRadiusNm);
      } else {
      auto it = config_.settings.find(settingKey);
      if (it != config_.settings.end()) {
        value = it->second;
      }
      }
    } else if (key == "pref.clock_24h") {
      value = RuntimeSettings::use24HourClock ? "true" : "false";
    } else if (key == "pref.temp_unit") {
      value = RuntimeSettings::useFahrenheit ? "F" : "C";
    } else if (key == "pref.distance_unit") {
      value = RuntimeSettings::useMiles ? "mi" : "km";
    }

    out = out.substring(0, start) + value + out.substring(end + 2);
    start = out.indexOf("{{");
  }

  return out;
}

uint32_t DslWidget::computeAdsbJitterMs(uint32_t pollMs) const {
  if (pollMs < 5000U) {
    return 0;
  }
  const uint32_t jitterMax = std::max(1000U, pollMs / 10U);
  return static_cast<uint32_t>(random(0, static_cast<long>(jitterMax + 1)));
}

bool DslWidget::applyFieldsFromDoc(const JsonDocument& doc, bool& changed) {
  int resolvedCount = 0;
  int missingCount = 0;
  int seriesCount = 0;

  for (const auto& pair : dsl_.fields) {
    const String& key = pair.first;
    const dsl::FieldSpec& spec = pair.second;
    const String path = bindRuntimeTemplate(spec.path);

    if (path.startsWith("computed.")) {
      String computed;
      bool ok = false;
      if (path == "computed.moon_phase") {
        ok = computeMoonPhaseName(computed);
      }

      if (!ok) {
        ++missingCount;
        if (values_[key] != "") {
          values_[key] = "";
          changed = true;
        }
        seriesValues_[key].clear();
        continue;
      }

      const String rawText = computed;
      dsl::FormatSpec resolvedFmt = spec.format;
      resolvedFmt.prefix = bindRuntimeTemplate(resolvedFmt.prefix);
      resolvedFmt.suffix = bindRuntimeTemplate(resolvedFmt.suffix);
      resolvedFmt.unit = bindRuntimeTemplate(resolvedFmt.unit);
      resolvedFmt.locale = bindRuntimeTemplate(resolvedFmt.locale);
      resolvedFmt.tz = bindRuntimeTemplate(resolvedFmt.tz);
      resolvedFmt.timeFormat = bindRuntimeTemplate(resolvedFmt.timeFormat);
      const String formatted = applyFormat(rawText, resolvedFmt, false, 0.0);

      if (values_[key] != formatted) {
        values_[key] = formatted;
        changed = true;
      }
      ++resolvedCount;
      continue;
    }

    JsonVariantConst v;
    if (!resolveVariant(doc, path, v)) {
      ++missingCount;
      if (dsl_.debug) {
        Serial.printf("[%s] - [%s] - DSL field miss key=%s path=%s\n", widgetName().c_str(),
                      logTimestamp().c_str(), key.c_str(), path.c_str());
      }
      if (values_[key] != "") {
        values_[key] = "";
        changed = true;
      }
      seriesValues_[key].clear();
      continue;
    }
    ++resolvedCount;

    if (v.is<JsonArrayConst>()) {
      ++seriesCount;
      const JsonArrayConst arr = v.as<JsonArrayConst>();
      std::vector<float> series;
      series.reserve(arr.size());
      for (JsonVariantConst el : arr) {
        if (el.is<float>() || el.is<double>() || el.is<long>() || el.is<int>()) {
          series.push_back(el.as<float>());
        }
      }

      if (seriesValues_[key] != series) {
        seriesValues_[key] = series;
        changed = true;
      }

      String lastText;
      if (!series.empty()) {
        lastText = applyFormat(String(series.back(), 2), spec.format, true, series.back());
      }
      if (values_[key] != lastText) {
        values_[key] = lastText;
        changed = true;
      }
      continue;
    }

    const bool numeric = v.is<float>() || v.is<double>() || v.is<long>() || v.is<int>();
    const double numericValue = numeric ? v.as<double>() : 0.0;
    const String rawText = toText(v);
    dsl::FormatSpec resolvedFmt = spec.format;
    resolvedFmt.prefix = bindRuntimeTemplate(resolvedFmt.prefix);
    resolvedFmt.suffix = bindRuntimeTemplate(resolvedFmt.suffix);
    resolvedFmt.unit = bindRuntimeTemplate(resolvedFmt.unit);
    resolvedFmt.locale = bindRuntimeTemplate(resolvedFmt.locale);
    resolvedFmt.tz = bindRuntimeTemplate(resolvedFmt.tz);
    resolvedFmt.timeFormat = bindRuntimeTemplate(resolvedFmt.timeFormat);
    const String formatted = applyFormat(rawText, resolvedFmt, numeric, numericValue);

    if (values_[key] != formatted) {
      values_[key] = formatted;
      changed = true;
    }
  }

  if (dsl_.debug) {
    Serial.printf("[%s] - [%s] - DSL parse summary resolved=%d missing=%d series=%d total=%u\n",
                  widgetName().c_str(), logTimestamp().c_str(), resolvedCount, missingCount,
                  seriesCount, static_cast<unsigned>(dsl_.fields.size()));
  }

  auto applyWeather = [&](const String& codeKey, const String& textKey, const String& iconKey) {
    auto it = values_.find(codeKey);
    if (it == values_.end() || it->second.isEmpty()) {
      if (values_[textKey] != "") {
        values_[textKey] = "";
        changed = true;
      }
      if (values_[iconKey] != "") {
        values_[iconKey] = "";
        changed = true;
      }
      return;
    }
    const int code = it->second.toInt();
    String text;
    String icon;
    mapWeatherCode(code, text, icon);
    if (!text.isEmpty() && values_[textKey] != text) {
      values_[textKey] = text;
      changed = true;
    }
    if (!icon.isEmpty() && values_[iconKey] != icon) {
      values_[iconKey] = icon;
      changed = true;
    }
  };

  applyWeather("code_now", "cond_now", "icon_now");
  applyWeather("day1_code", "day1_cond", "day1_icon");
  applyWeather("day2_code", "day2_cond", "day2_icon");

  return true;
}

bool DslWidget::computeMoonPhaseName(String& out) const {
  float phase = 0.0f;
  if (!computeMoonPhaseFraction(phase)) {
    return false;
  }

  if (phase < 0.0625f || phase >= 0.9375f) {
    out = "New Moon";
  } else if (phase < 0.1875f) {
    out = "Waxing Crescent";
  } else if (phase < 0.3125f) {
    out = "First Quarter";
  } else if (phase < 0.4375f) {
    out = "Waxing Gibbous";
  } else if (phase < 0.5625f) {
    out = "Full Moon";
  } else if (phase < 0.6875f) {
    out = "Waning Gibbous";
  } else if (phase < 0.8125f) {
    out = "Last Quarter";
  } else {
    out = "Waning Crescent";
  }
  return true;
}

bool DslWidget::computeMoonPhaseFraction(float& out) const {
  const time_t nowUtc = time(nullptr);
  if (nowUtc < 946684800) {
    return false;
  }

  struct tm nowTm;
  gmtime_r(&nowUtc, &nowTm);

  const double daysNow =
      static_cast<double>(daysFromCivil(nowTm.tm_year + 1900, nowTm.tm_mon + 1, nowTm.tm_mday)) +
      (static_cast<double>(nowTm.tm_hour) +
       static_cast<double>(nowTm.tm_min) / 60.0 +
       static_cast<double>(nowTm.tm_sec) / 3600.0) /
          24.0;

  const double epochDays =
      static_cast<double>(daysFromCivil(2000, 1, 6)) + (18.0 + 14.0 / 60.0) / 24.0;
  const double synodic = 29.53058867;
  double age = daysNow - epochDays;
  age = fmod(age, synodic);
  if (age < 0) {
    age += synodic;
  }
  out = static_cast<float>(age / synodic);
  return true;
}

bool DslWidget::resolveVariant(const JsonDocument& doc, const String& path,
                               JsonVariantConst& out) const {
  JsonVariantConst current = doc.as<JsonVariantConst>();
  int segStart = 0;

  while (segStart < path.length()) {
    int segEnd = path.indexOf('.', segStart);
    if (segEnd < 0) {
      segEnd = path.length();
    }

    const String seg = path.substring(segStart, segEnd);
    if (seg.isEmpty()) {
      return false;
    }

    int pos = 0;
    while (pos < seg.length() && seg[pos] != '[') {
      ++pos;
    }

    const String key = seg.substring(0, pos);
    if (!key.isEmpty()) {
      if (!current.is<JsonObjectConst>()) {
        return false;
      }
      current = current[key];
      if (current.isNull()) {
        return false;
      }
    }

    while (pos < seg.length()) {
      if (seg[pos] != '[') {
        return false;
      }
      const int close = seg.indexOf(']', pos + 1);
      if (close < 0) {
        return false;
      }

      const String idxStr = seg.substring(pos + 1, close);
      if (idxStr.isEmpty()) {
        return false;
      }

      const int idx = idxStr.toInt();
      if (!current.is<JsonArrayConst>()) {
        return false;
      }

      const JsonArrayConst arr = current.as<JsonArrayConst>();
      if (idx < 0 || idx >= static_cast<int>(arr.size())) {
        return false;
      }

      current = arr[idx];
      if (current.isNull()) {
        return false;
      }
      pos = close + 1;
    }

    segStart = segEnd + 1;
  }

  out = current;
  return !out.isNull();
}

String DslWidget::toText(JsonVariantConst value) const {
  if (value.is<const char*>()) {
    return String(value.as<const char*>());
  }
  if (value.is<long>()) {
    return String(value.as<long>());
  }
  if (value.is<unsigned long>()) {
    return String(value.as<unsigned long>());
  }
  if (value.is<float>()) {
    return String(value.as<float>(), 2);
  }
  if (value.is<double>()) {
    return String(value.as<double>(), 2);
  }
  if (value.is<bool>()) {
    return value.as<bool>() ? "true" : "false";
  }
  return String();
}

String DslWidget::bindTemplate(const String& input) const {
  String out = input;
  int start = out.indexOf("{{");

  while (start >= 0) {
    const int end = out.indexOf("}}", start + 2);
    if (end < 0) {
      break;
    }

    const String key = out.substring(start + 2, end);
    auto it = values_.find(key);
    if (it != values_.end()) {
      out = out.substring(0, start) + it->second + out.substring(end + 2);
      start = out.indexOf("{{");
    } else {
      start = out.indexOf("{{", end + 2);
    }
  }

  return bindRuntimeTemplate(out);
}
