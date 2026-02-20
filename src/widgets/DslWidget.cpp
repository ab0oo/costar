#include "widgets/DslWidget.h"

#include "RuntimeGeo.h"
#include "RuntimeSettings.h"
#include "dsl/DslParser.h"

DslWidget::DslWidget(const WidgetConfig& cfg) : Widget(cfg) {
  auto pathIt = config_.settings.find("dsl_path");
  if (pathIt != config_.settings.end()) {
    dslPath_ = pathIt->second;
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

void DslWidget::begin() {
  Widget::begin();
  dslLoaded_ = loadDslModel();
  if (dslLoaded_) {
    // Force first DSL fetch immediately; do not wait full poll interval.
    const uint32_t nowMs = millis();
    lastFetchMs_ = (nowMs > dsl_.pollMs) ? (nowMs - dsl_.pollMs) : 0;
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
    } else if (key == "geo.offset_min") {
      value = String(RuntimeGeo::utcOffsetMinutes);
    } else if (key.startsWith("setting.")) {
      const String settingKey = key.substring(8);
      auto it = config_.settings.find(settingKey);
      if (it != config_.settings.end()) {
        value = it->second;
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

bool DslWidget::applyFieldsFromDoc(const JsonDocument& doc, bool& changed) {
  int resolvedCount = 0;
  int missingCount = 0;
  int seriesCount = 0;

  for (const auto& pair : dsl_.fields) {
    const String& key = pair.first;
    const dsl::FieldSpec& spec = pair.second;
    const String path = bindRuntimeTemplate(spec.path);

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
    const String value = (it != values_.end()) ? it->second : String();

    out = out.substring(0, start) + value + out.substring(end + 2);
    start = out.indexOf("{{");
  }

  return bindRuntimeTemplate(out);
}
