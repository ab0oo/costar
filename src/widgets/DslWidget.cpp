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

  auto delayIt = config_.settings.find("start_delay_ms");
  if (delayIt != config_.settings.end()) {
    const long parsed = delayIt->second.toInt();
    if (parsed > 0) {
      startDelayMs_ = static_cast<uint32_t>(parsed);
    }
  }

  hasTapHttpAction_ = !parseTapActionType().isEmpty();
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
    firstFetchNotBeforeMs_ = nowMs + startDelayMs_;
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
  pathValues_.clear();
  if (debugOverride_) {
    dsl_.debug = true;
  }
  status_ = "dsl ok";
  return true;
}

String DslWidget::parseTapActionType() const {
  auto it = config_.settings.find("tap_action");
  if (it == config_.settings.end()) {
    return String();
  }
  String action = it->second;
  action.trim();
  action.toLowerCase();
  if (action == "refresh" || action == "http") {
    return action;
  }
  return String();
}

bool DslWidget::onTouch(uint16_t localX, uint16_t localY, TouchType type) {
  (void)localX;
  (void)localY;
  if (type != TouchType::kTap || !dslLoaded_) {
    return false;
  }

  const String action = parseTapActionType();
  if (action.isEmpty()) {
    return false;
  }

  if (action == "refresh") {
    forceFetchNow_ = true;
    return true;
  }

  if (action == "http") {
    auto urlIt = config_.settings.find("tap_url");
    if (urlIt == config_.settings.end() || urlIt->second.isEmpty()) {
      return false;
    }
    tapActionPending_ = true;
    return true;
  }

  return false;
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

std::map<String, String> DslWidget::resolveHttpHeaders() const {
  std::map<String, String> resolved;
  for (const auto& kv : dsl_.headers) {
    String key = kv.first;
    key.trim();
    if (key.isEmpty()) {
      continue;
    }
    const String value = bindRuntimeTemplate(kv.second);
    if (value.isEmpty()) {
      continue;
    }
    resolved[key] = value;
  }
  return resolved;
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

  for (const auto& node : dsl_.nodes) {
    if (node.type != dsl::NodeType::kLabel || node.path.isEmpty()) {
      continue;
    }
    const String path = bindRuntimeTemplate(node.path);
    JsonVariantConst v;
    String text;
    if (resolveVariant(doc, path, v)) {
      text = toText(v);
    }

    if (pathValues_[node.path] != text) {
      pathValues_[node.path] = text;
      changed = true;
    }
  }

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

bool DslWidget::resolveVariantPath(const JsonVariantConst& root, const String& path,
                                   JsonVariantConst& out) const {
  String workPath = path;
  workPath.trim();
  if (workPath.isEmpty()) {
    out = root;
    return !out.isNull();
  }

  JsonVariantConst current = root;
  int segStart = 0;

  while (segStart < workPath.length()) {
    int segEnd = workPath.indexOf('.', segStart);
    if (segEnd < 0) {
      segEnd = workPath.length();
    }

    const String seg = workPath.substring(segStart, segEnd);
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
      for (int i = 0; i < idxStr.length(); ++i) {
        const char c = idxStr[i];
        if (c < '0' || c > '9') {
          return false;
        }
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

bool DslWidget::resolveSortVariant(const JsonDocument& doc, const String& path,
                                   JsonVariantConst& out) const {
  bool numericSort = false;
  bool distanceSort = false;
  int argsStart = -1;
  if (path.startsWith("sort_num(")) {
    numericSort = true;
    argsStart = 9;
  } else if (path.startsWith("sort_alpha(")) {
    numericSort = false;
    argsStart = 11;
  } else if (path.startsWith("distance_sort(")) {
    distanceSort = true;
    argsStart = 14;
  } else if (path.startsWith("sort_distance(")) {
    distanceSort = true;
    argsStart = 14;
  } else {
    return false;
  }

  const int close = path.indexOf(')', argsStart);
  if (close < 0) {
    return false;
  }

  String argsRaw = path.substring(argsStart, close);
  std::vector<String> args;
  int start = 0;
  while (start <= argsRaw.length()) {
    int comma = argsRaw.indexOf(',', start);
    if (comma < 0) {
      comma = argsRaw.length();
    }
    String part = argsRaw.substring(start, comma);
    part.trim();
    args.push_back(part);
    if (comma >= argsRaw.length()) {
      break;
    }
    start = comma + 1;
  }

  const String arrayPath = args[0];
  if (arrayPath.isEmpty()) {
    return false;
  }

  String keyPath;
  float originLat = 0.0f;
  float originLon = 0.0f;
  String order = "asc";

  if (distanceSort) {
    if (args.size() < 3 || args.size() > 4) {
      return false;
    }
    auto parseNumberArg = [&](const String& arg, float& outNum) -> bool {
      String trimmed = arg;
      trimmed.trim();
      if (trimmed.isEmpty()) {
        return false;
      }

      bool hasDigit = false;
      bool hasAlpha = false;
      for (int i = 0; i < trimmed.length(); ++i) {
        const char c = trimmed[i];
        if (c >= '0' && c <= '9') {
          hasDigit = true;
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
          hasAlpha = true;
        }
      }
      if (hasDigit && !hasAlpha) {
        outNum = trimmed.toFloat();
        return true;
      }

      JsonVariantConst v;
      if (!resolveVariantPath(doc.as<JsonVariantConst>(), trimmed, v)) {
        return false;
      }
      if (v.is<float>() || v.is<double>() || v.is<long>() || v.is<int>()) {
        outNum = v.as<float>();
        return true;
      }
      if (v.is<const char*>()) {
        String text = v.as<String>();
        text.trim();
        bool digit = false;
        for (int i = 0; i < text.length(); ++i) {
          if (text[i] >= '0' && text[i] <= '9') {
            digit = true;
            break;
          }
        }
        if (digit) {
          outNum = text.toFloat();
          return true;
        }
      }
      return false;
    };

    if (!parseNumberArg(args[1], originLat) || !parseNumberArg(args[2], originLon)) {
      return false;
    }
    order = args.size() == 4 ? args[3] : String("asc");
  } else {
    if (args.size() < 2 || args.size() > 3) {
      return false;
    }
    keyPath = args[1];
    order = args.size() == 3 ? args[2] : String("asc");
  }

  order.toLowerCase();
  const bool descending = (order == "desc" || order == "reverse" || order == "rev");

  String tail = path.substring(close + 1);
  tail.trim();
  if (tail.startsWith(".")) {
    tail = tail.substring(1);
  }

  JsonVariantConst arrVariant;
  if (!resolveVariantPath(doc.as<JsonVariantConst>(), arrayPath, arrVariant) ||
      !arrVariant.is<JsonArrayConst>()) {
    return false;
  }
  const JsonArrayConst arr = arrVariant.as<JsonArrayConst>();

  auto resolveSortKey = [&](JsonVariantConst item, JsonVariantConst& keyOut) -> bool {
    if (keyPath.isEmpty() || keyPath == "." || keyPath == "*") {
      keyOut = item;
      return !keyOut.isNull();
    }
    return resolveVariantPath(item, keyPath, keyOut);
  };

  auto numericOf = [&](JsonVariantConst v, float& outNum) -> bool {
    if (v.is<float>() || v.is<double>() || v.is<long>() || v.is<int>()) {
      outNum = v.as<float>();
      return true;
    }
    if (v.is<const char*>()) {
      const String text = v.as<String>();
      String filtered;
      filtered.reserve(text.length());
      bool hasDigit = false;
      for (int i = 0; i < text.length(); ++i) {
        const char c = text[i];
        if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+') {
          filtered += c;
          if (c >= '0' && c <= '9') {
            hasDigit = true;
          }
        }
      }
      if (!hasDigit) {
        return false;
      }
      outNum = filtered.toFloat();
      return true;
    }
    return false;
  };

  auto textOf = [&](JsonVariantConst v) -> String {
    if (v.is<const char*>()) {
      return v.as<String>();
    }
    if (v.is<float>() || v.is<double>() || v.is<long>() || v.is<int>()) {
      return String(v.as<double>(), 3);
    }
    if (v.is<bool>()) {
      return v.as<bool>() ? "true" : "false";
    }
    return String();
  };

  auto distanceMetersOf = [&](JsonVariantConst item, float& outMeters) -> bool {
    if (!item.is<JsonObjectConst>()) {
      return false;
    }
    JsonObjectConst obj = item.as<JsonObjectConst>();
    JsonVariantConst latV = obj["lat"];
    JsonVariantConst lonV = obj["lon"];
    if (latV.isNull() || lonV.isNull()) {
      return false;
    }

    float lat = 0.0f;
    float lon = 0.0f;
    if (latV.is<float>() || latV.is<double>() || latV.is<long>() || latV.is<int>()) {
      lat = latV.as<float>();
    } else if (latV.is<const char*>()) {
      const String s = latV.as<String>();
      if (s.length() == 0) return false;
      lat = s.toFloat();
    } else {
      return false;
    }

    if (lonV.is<float>() || lonV.is<double>() || lonV.is<long>() || lonV.is<int>()) {
      lon = lonV.as<float>();
    } else if (lonV.is<const char*>()) {
      const String s = lonV.as<String>();
      if (s.length() == 0) return false;
      lon = s.toFloat();
    } else {
      return false;
    }

    outMeters = distanceKm(originLat, originLon, lat, lon) * 1000.0f;
    return true;
  };

  std::vector<int> indexOrder(arr.size());
  for (int i = 0; i < static_cast<int>(arr.size()); ++i) {
    indexOrder[i] = i;
  }

  auto cmpAsc = [&](int lhs, int rhs) -> bool {
    JsonVariantConst leftKey;
    JsonVariantConst rightKey;
    const bool haveLeft = resolveSortKey(arr[lhs], leftKey);
    const bool haveRight = resolveSortKey(arr[rhs], rightKey);

    if (distanceSort) {
      float ldist = 0.0f;
      float rdist = 0.0f;
      const bool lOk = distanceMetersOf(arr[lhs], ldist);
      const bool rOk = distanceMetersOf(arr[rhs], rdist);
      if (lOk && rOk) {
        if (fabsf(ldist - rdist) > 0.000001f) {
          return ldist < rdist;
        }
        return lhs < rhs;
      }
      if (lOk != rOk) {
        return lOk;
      }
      return lhs < rhs;
    }

    if (numericSort) {
      float lnum = 0.0f;
      float rnum = 0.0f;
      const bool lOk = haveLeft && numericOf(leftKey, lnum);
      const bool rOk = haveRight && numericOf(rightKey, rnum);
      if (lOk && rOk) {
        if (fabsf(lnum - rnum) > 0.000001f) {
          return lnum < rnum;
        }
        return lhs < rhs;
      }
      if (lOk != rOk) {
        return lOk;
      }
      return lhs < rhs;
    }

    String ls = haveLeft ? textOf(leftKey) : String();
    String rs = haveRight ? textOf(rightKey) : String();
    ls.toLowerCase();
    rs.toLowerCase();
    const int cmp = ls.compareTo(rs);
    if (cmp != 0) {
      return cmp < 0;
    }
    return lhs < rhs;
  };

  std::stable_sort(indexOrder.begin(), indexOrder.end(), [&](int lhs, int rhs) {
    return descending ? cmpAsc(rhs, lhs) : cmpAsc(lhs, rhs);
  });

  transformDoc_.clear();
  JsonArray sorted = transformDoc_.to<JsonArray>();
  for (int idx : indexOrder) {
    sorted.add(arr[idx]);
  }

  const JsonVariantConst sortedRoot = transformDoc_.as<JsonVariantConst>();
  if (tail.isEmpty()) {
    out = sortedRoot;
    return !out.isNull();
  }
  return resolveVariantPath(sortedRoot, tail, out);
}

bool DslWidget::resolveVariant(const JsonDocument& doc, const String& path,
                               JsonVariantConst& out) const {
  String workPath = path;
  workPath.trim();
  if (workPath.isEmpty()) {
    return false;
  }

  if (workPath.startsWith("sort_num(") || workPath.startsWith("sort_alpha(") ||
      workPath.startsWith("distance_sort(") || workPath.startsWith("sort_distance(")) {
    return resolveSortVariant(doc, workPath, out);
  }
  return resolveVariantPath(doc.as<JsonVariantConst>(), workPath, out);
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
