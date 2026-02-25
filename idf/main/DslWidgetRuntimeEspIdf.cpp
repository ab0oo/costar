#include "DslWidgetRuntimeEspIdf.h"

#include "AppConfig.h"
#include "DisplaySpiEspIdf.h"
#include "Font5x7Classic.h"
#include "RuntimeSettings.h"
#include "platform/Platform.h"
#include "platform/Prefs.h"

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
constexpr const char* kTag = "dsl-widget";
constexpr uint16_t kBg = 0x0000;
constexpr uint16_t kText = 0xFFFF;
constexpr uint16_t kAccent = 0x9FD3;
constexpr uint16_t kBorder = 0x39E7;
constexpr uint32_t kDefaultPollMs = 180000U;
constexpr uint32_t kInitialPollMs = 15000U;
constexpr uint32_t kHttpGateTimeoutMs = 7000U;
constexpr uint32_t kHttpTimeoutMs = 6500U;

enum class DataSource : uint8_t {
  kHttp,
  kLocalTime,
  kUnknown,
};

struct FormatSpec {
  int roundDigits = -1;
  std::string unit;
  std::string locale;
  std::string prefix;
  std::string suffix;
  std::string tz;
  std::string timeFormat;
};

struct FieldSpec {
  std::string key;
  std::string path;
  FormatSpec format;
};

struct LabelNode {
  int x = 0;
  int y = 0;
  int font = 1;
  std::string text;
};

struct KeyValue {
  std::string key;
  std::string value;
};

struct HttpCapture {
  std::string body;
};

struct LocalTimeContext {
  int year = 0;
  int mon = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  int utcOffsetMinutes = 0;
  bool hasOffset = false;
  std::string timezone;
  std::string date;
  std::string time24;
  std::string time12;
  std::string isoLocal;
};

struct State {
  bool active = false;
  bool hasData = false;
  bool debug = false;
  std::string widgetId;
  std::string dslPath;
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  DataSource source = DataSource::kUnknown;
  uint32_t pollMs = kDefaultPollMs;
  uint32_t lastFetchMs = 0;
  uint32_t backoffUntilMs = 0;
  uint8_t failureStreak = 0;
  std::string urlTemplate;
  std::vector<FieldSpec> fields;
  std::vector<LabelNode> labels;
  std::vector<KeyValue> values;
};

State s;
SemaphoreHandle_t sHttpGate = nullptr;

size_t skipWs(std::string_view text, size_t i) {
  while (i < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
      break;
    }
    ++i;
  }
  return i;
}

std::string_view trimView(std::string_view text) {
  size_t start = 0;
  while (start < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[start]);
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
      break;
    }
    ++start;
  }
  size_t end = text.size();
  while (end > start) {
    const unsigned char c = static_cast<unsigned char>(text[end - 1]);
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
      break;
    }
    --end;
  }
  return text.substr(start, end - start);
}

bool parseQuotedString(std::string_view text, size_t quotePos, std::string& out, size_t& nextPos) {
  if (quotePos >= text.size() || text[quotePos] != '"') {
    return false;
  }
  out.clear();

  for (size_t i = quotePos + 1; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '"') {
      nextPos = i + 1;
      return true;
    }
    if (c == '\\') {
      if (i + 1 >= text.size()) {
        return false;
      }
      const char esc = text[++i];
      switch (esc) {
        case '"':
        case '\\':
        case '/':
          out.push_back(esc);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          // Keep parsing lightweight: decode BMP ASCII codepoints, degrade others to '?'.
          if (i + 4 >= text.size()) {
            return false;
          }
          unsigned value = 0;
          for (int nib = 0; nib < 4; ++nib) {
            const char hex = text[i + 1 + static_cast<size_t>(nib)];
            value <<= 4;
            if (hex >= '0' && hex <= '9') {
              value |= static_cast<unsigned>(hex - '0');
            } else if (hex >= 'A' && hex <= 'F') {
              value |= static_cast<unsigned>(hex - 'A' + 10);
            } else if (hex >= 'a' && hex <= 'f') {
              value |= static_cast<unsigned>(hex - 'a' + 10);
            } else {
              return false;
            }
          }
          i += 4;
          out.push_back((value <= 0x7F) ? static_cast<char>(value) : '?');
          break;
        }
        default:
          out.push_back(esc);
          break;
      }
      continue;
    }
    out.push_back(c);
  }

  return false;
}

bool findValueEnd(std::string_view text, size_t start, size_t& endPos) {
  start = skipWs(text, start);
  if (start >= text.size()) {
    return false;
  }

  const char first = text[start];
  if (first == '"') {
    std::string ignored;
    size_t next = 0;
    if (!parseQuotedString(text, start, ignored, next)) {
      return false;
    }
    endPos = next;
    return true;
  }

  if (first == '{' || first == '[') {
    const char open = first;
    const char close = (first == '{') ? '}' : ']';
    int depth = 0;
    bool inString = false;
    bool escape = false;
    for (size_t i = start; i < text.size(); ++i) {
      const char c = text[i];
      if (inString) {
        if (escape) {
          escape = false;
        } else if (c == '\\') {
          escape = true;
        } else if (c == '"') {
          inString = false;
        }
        continue;
      }

      if (c == '"') {
        inString = true;
        continue;
      }
      if (c == open) {
        ++depth;
        continue;
      }
      if (c == close) {
        --depth;
        if (depth == 0) {
          endPos = i + 1;
          return true;
        }
      }
    }
    return false;
  }

  size_t i = start;
  while (i < text.size()) {
    const char c = text[i];
    if (c == ',' || c == '}' || c == ']') {
      break;
    }
    ++i;
  }
  endPos = i;
  return true;
}

bool objectMemberValue(std::string_view objectText, std::string_view key, std::string_view& out) {
  objectText = trimView(objectText);
  if (objectText.size() < 2 || objectText.front() != '{' || objectText.back() != '}') {
    return false;
  }

  size_t i = 1;
  while (i + 1 < objectText.size()) {
    i = skipWs(objectText, i);
    if (i >= objectText.size() - 1 || objectText[i] == '}') {
      break;
    }

    if (objectText[i] != '"') {
      return false;
    }

    std::string memberKey;
    size_t keyEnd = 0;
    if (!parseQuotedString(objectText, i, memberKey, keyEnd)) {
      return false;
    }

    i = skipWs(objectText, keyEnd);
    if (i >= objectText.size() || objectText[i] != ':') {
      return false;
    }

    ++i;
    const size_t valueStart = skipWs(objectText, i);
    size_t valueEnd = 0;
    if (!findValueEnd(objectText, valueStart, valueEnd)) {
      return false;
    }

    if (memberKey == key) {
      out = objectText.substr(valueStart, valueEnd - valueStart);
      return true;
    }

    i = skipWs(objectText, valueEnd);
    if (i < objectText.size() && objectText[i] == ',') {
      ++i;
    }
  }

  return false;
}

template <typename Fn>
void forEachObjectMember(std::string_view objectText, Fn&& fn) {
  objectText = trimView(objectText);
  if (objectText.size() < 2 || objectText.front() != '{' || objectText.back() != '}') {
    return;
  }

  size_t i = 1;
  while (i + 1 < objectText.size()) {
    i = skipWs(objectText, i);
    if (i >= objectText.size() - 1 || objectText[i] == '}') {
      break;
    }
    if (objectText[i] != '"') {
      return;
    }

    std::string memberKey;
    size_t keyEnd = 0;
    if (!parseQuotedString(objectText, i, memberKey, keyEnd)) {
      return;
    }

    i = skipWs(objectText, keyEnd);
    if (i >= objectText.size() || objectText[i] != ':') {
      return;
    }

    ++i;
    const size_t valueStart = skipWs(objectText, i);
    size_t valueEnd = 0;
    if (!findValueEnd(objectText, valueStart, valueEnd)) {
      return;
    }

    const std::string_view valueText = objectText.substr(valueStart, valueEnd - valueStart);
    fn(memberKey, valueText);

    i = skipWs(objectText, valueEnd);
    if (i < objectText.size() && objectText[i] == ',') {
      ++i;
    }
  }
}

bool arrayElementValue(std::string_view arrayText, int index, std::string_view& out) {
  if (index < 0) {
    return false;
  }

  arrayText = trimView(arrayText);
  if (arrayText.size() < 2 || arrayText.front() != '[' || arrayText.back() != ']') {
    return false;
  }

  int current = 0;
  size_t i = 1;
  while (i + 1 < arrayText.size()) {
    i = skipWs(arrayText, i);
    if (i >= arrayText.size() - 1 || arrayText[i] == ']') {
      break;
    }

    size_t valueEnd = 0;
    if (!findValueEnd(arrayText, i, valueEnd)) {
      return false;
    }
    if (current == index) {
      out = arrayText.substr(i, valueEnd - i);
      return true;
    }

    ++current;
    i = skipWs(arrayText, valueEnd);
    if (i < arrayText.size() && arrayText[i] == ',') {
      ++i;
    }
  }

  return false;
}

template <typename Fn>
void forEachArrayElement(std::string_view arrayText, Fn&& fn) {
  arrayText = trimView(arrayText);
  if (arrayText.size() < 2 || arrayText.front() != '[' || arrayText.back() != ']') {
    return;
  }

  int idx = 0;
  size_t i = 1;
  while (i + 1 < arrayText.size()) {
    i = skipWs(arrayText, i);
    if (i >= arrayText.size() - 1 || arrayText[i] == ']') {
      break;
    }

    size_t valueEnd = 0;
    if (!findValueEnd(arrayText, i, valueEnd)) {
      return;
    }

    fn(idx, arrayText.substr(i, valueEnd - i));
    ++idx;

    i = skipWs(arrayText, valueEnd);
    if (i < arrayText.size() && arrayText[i] == ',') {
      ++i;
    }
  }
}

bool parseStrictDouble(const std::string& text, double& out) {
  std::string trimmed = text;
  const auto notWs = [](unsigned char c) { return c != ' ' && c != '\n' && c != '\r' && c != '\t'; };
  trimmed.erase(trimmed.begin(), std::find_if(trimmed.begin(), trimmed.end(), notWs));
  trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(), notWs).base(), trimmed.end());
  if (trimmed.empty()) {
    return false;
  }

  errno = 0;
  char* endPtr = nullptr;
  const double value = std::strtod(trimmed.c_str(), &endPtr);
  if (endPtr == nullptr || endPtr == trimmed.c_str() || errno == ERANGE) {
    return false;
  }
  while (*endPtr == ' ' || *endPtr == '\n' || *endPtr == '\r' || *endPtr == '\t') {
    ++endPtr;
  }
  if (*endPtr != '\0') {
    return false;
  }

  out = value;
  return true;
}

bool viewToString(std::string_view valueText, std::string& out) {
  valueText = trimView(valueText);
  if (valueText.empty()) {
    out.clear();
    return false;
  }

  if (valueText.front() == '"') {
    size_t next = 0;
    if (!parseQuotedString(valueText, 0, out, next)) {
      return false;
    }
    return true;
  }

  out.assign(valueText.data(), valueText.size());
  return true;
}

bool viewToInt(std::string_view valueText, int& out) {
  std::string text;
  if (!viewToString(valueText, text)) {
    return false;
  }
  double value = 0.0;
  if (!parseStrictDouble(text, value)) {
    return false;
  }
  if (value < static_cast<double>(std::numeric_limits<int>::min()) ||
      value > static_cast<double>(std::numeric_limits<int>::max())) {
    return false;
  }
  out = static_cast<int>(std::lround(value));
  return true;
}

bool viewToBool(std::string_view valueText, bool& out) {
  std::string text;
  if (!viewToString(valueText, text)) {
    return false;
  }
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (text == "true" || text == "1" || text == "yes" || text == "on") {
    out = true;
    return true;
  }
  if (text == "false" || text == "0" || text == "no" || text == "off") {
    out = false;
    return true;
  }
  return false;
}

bool viewToDouble(std::string_view valueText, double& out) {
  valueText = trimView(valueText);
  if (valueText.empty()) {
    return false;
  }
  if (valueText.front() == '"') {
    std::string str;
    size_t next = 0;
    if (!parseQuotedString(valueText, 0, str, next)) {
      return false;
    }
    return parseStrictDouble(str, out);
  }
  std::string str(valueText.data(), valueText.size());
  return parseStrictDouble(str, out);
}

bool objectMemberString(std::string_view objectText, const char* key, std::string& out) {
  std::string_view value;
  if (!objectMemberValue(objectText, key, value)) {
    return false;
  }
  return viewToString(value, out);
}

bool objectMemberInt(std::string_view objectText, const char* key, int& out) {
  std::string_view value;
  if (!objectMemberValue(objectText, key, value)) {
    return false;
  }
  return viewToInt(value, out);
}

bool objectMemberBool(std::string_view objectText, const char* key, bool& out) {
  std::string_view value;
  if (!objectMemberValue(objectText, key, value)) {
    return false;
  }
  return viewToBool(value, out);
}

bool objectMemberObject(std::string_view objectText, const char* key, std::string_view& out) {
  if (!objectMemberValue(objectText, key, out)) {
    return false;
  }
  out = trimView(out);
  return out.size() >= 2 && out.front() == '{' && out.back() == '}';
}

bool objectMemberArray(std::string_view objectText, const char* key, std::string_view& out) {
  if (!objectMemberValue(objectText, key, out)) {
    return false;
  }
  out = trimView(out);
  return out.size() >= 2 && out.front() == '[' && out.back() == ']';
}

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

KeyValue* findValueSlot(const std::string& key) {
  for (auto& kv : s.values) {
    if (kv.key == key) {
      return &kv;
    }
  }
  return nullptr;
}

const std::string* getValue(const std::string& key) {
  for (const auto& kv : s.values) {
    if (kv.key == key) {
      return &kv.value;
    }
  }
  return nullptr;
}

bool setValue(const std::string& key, const std::string& value) {
  if (KeyValue* slot = findValueSlot(key); slot != nullptr) {
    if (slot->value == value) {
      return false;
    }
    slot->value = value;
    return true;
  }
  s.values.push_back({key, value});
  return true;
}

std::string replaceAll(std::string input, const std::string& needle, const std::string& value) {
  if (needle.empty()) {
    return input;
  }
  size_t pos = 0;
  while ((pos = input.find(needle, pos)) != std::string::npos) {
    input.replace(pos, needle.size(), value);
    pos += value.size();
  }
  return input;
}

std::string trimCopy(const std::string& in) {
  const auto notWs = [](unsigned char c) { return c != ' ' && c != '\n' && c != '\r' && c != '\t'; };
  auto begin = std::find_if(in.begin(), in.end(), notWs);
  auto end = std::find_if(in.rbegin(), in.rend(), notWs).base();
  if (begin >= end) {
    return {};
  }
  return std::string(begin, end);
}

std::string unquoteCopy(std::string token) {
  token = trimCopy(token);
  if (token.size() >= 2) {
    const char first = token.front();
    const char last = token.back();
    if ((first == '\'' && last == '\'') || (first == '"' && last == '"')) {
      return token.substr(1, token.size() - 2);
    }
  }
  return token;
}

void splitArgs(const std::string& raw, std::vector<std::string>& out) {
  out.clear();
  std::string current;
  int depth = 0;
  char quote = '\0';
  for (char c : raw) {
    if (quote != '\0') {
      current.push_back(c);
      if (c == quote) {
        quote = '\0';
      }
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
      current.push_back(c);
      continue;
    }
    if (c == '(') {
      ++depth;
      current.push_back(c);
      continue;
    }
    if (c == ')') {
      if (depth > 0) {
        --depth;
      }
      current.push_back(c);
      continue;
    }
    if (c == ',' && depth == 0) {
      out.push_back(trimCopy(current));
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  out.push_back(trimCopy(current));
}

std::string resolveKnownToken(const std::string& key, bool* found) {
  if (found != nullptr) {
    *found = true;
  }

  if (key == "geo.lat") {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", loadGeoLat());
    return std::string(buf);
  }
  if (key == "geo.lon") {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", loadGeoLon());
    return std::string(buf);
  }
  if (key == "geo.tz") {
    return loadGeoTimezone();
  }
  if (key == "geo.label") {
    const std::string tz = loadGeoTimezone();
    return tz.empty() ? std::string("Unknown") : tz;
  }
  if (key == "geo.offset_min") {
    int offset = 0;
    if (loadGeoOffsetMinutes(offset)) {
      return std::to_string(offset);
    }
    return "0";
  }

  if (key == "pref.clock_24h") {
    return RuntimeSettings::use24HourClock ? "true" : "false";
  }
  if (key == "pref.temp_unit") {
    return RuntimeSettings::useFahrenheit ? "F" : "C";
  }
  if (key == "pref.distance_unit") {
    return RuntimeSettings::useMiles ? "mi" : "km";
  }

  if (const std::string* value = getValue(key); value != nullptr) {
    return *value;
  }

  if (found != nullptr) {
    *found = false;
  }
  return {};
}

std::string resolveArgValue(const std::string& arg) {
  const std::string token = unquoteCopy(arg);
  bool found = false;
  const std::string known = resolveKnownToken(token, &found);
  return found ? known : token;
}

bool parseNumericArg(const std::string& arg, double& out) {
  const std::string value = resolveArgValue(arg);
  return parseStrictDouble(value, out);
}

std::string bindRuntimeTemplate(const std::string& input) {
  std::string out = input;

  size_t start = out.find("{{");
  while (start != std::string::npos) {
    const size_t end = out.find("}}", start + 2);
    if (end == std::string::npos) {
      break;
    }

    const std::string expr = trimCopy(out.substr(start + 2, end - start - 2));
    std::string value;
    bool resolved = false;

    const size_t lparen = expr.find('(');
    if (lparen != std::string::npos && !expr.empty() && expr.back() == ')') {
      std::string fn = trimCopy(expr.substr(0, lparen));
      std::transform(fn.begin(), fn.end(), fn.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      const std::string rawArgs = expr.substr(lparen + 1, expr.size() - lparen - 2);
      std::vector<std::string> args;
      splitArgs(rawArgs, args);

      if ((fn == "if_true" && args.size() == 3) ||
          ((fn == "if_eq" || fn == "if_ne" || fn == "if_gt" || fn == "if_gte" ||
            fn == "if_lt" || fn == "if_lte") &&
           args.size() == 4)) {
        if (fn == "if_true") {
          std::string cond = resolveArgValue(args[0]);
          std::string condLower = cond;
          std::transform(condLower.begin(), condLower.end(), condLower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
          });
          const bool truthy = !cond.empty() && condLower != "0" && condLower != "false" &&
                              condLower != "no" && condLower != "off";
          value = truthy ? resolveArgValue(args[1]) : resolveArgValue(args[2]);
          resolved = true;
        } else if (fn == "if_eq" || fn == "if_ne") {
          const std::string lhs = resolveArgValue(args[0]);
          const std::string rhs = resolveArgValue(args[1]);
          const bool eq = lhs == rhs;
          value = ((fn == "if_eq") == eq) ? resolveArgValue(args[2]) : resolveArgValue(args[3]);
          resolved = true;
        } else {
          double lhs = 0.0;
          double rhs = 0.0;
          if (parseNumericArg(args[0], lhs) && parseNumericArg(args[1], rhs)) {
            bool cond = false;
            if (fn == "if_gt") {
              cond = lhs > rhs;
            } else if (fn == "if_gte") {
              cond = lhs >= rhs;
            } else if (fn == "if_lt") {
              cond = lhs < rhs;
            } else if (fn == "if_lte") {
              cond = lhs <= rhs;
            }
            value = cond ? resolveArgValue(args[2]) : resolveArgValue(args[3]);
            resolved = true;
          }
        }
      }
    }

    if (!resolved) {
      bool found = false;
      value = resolveKnownToken(expr, &found);
      resolved = found;
    }

    if (!resolved) {
      value.clear();
    }

    out.replace(start, end - start + 2, value);
    start = out.find("{{");
  }

  return out;
}

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
  if (!parseStrictDouble(hhText, hh) || !parseStrictDouble(mmText, mm)) {
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
  if (!parseStrictDouble(y, yv) || !parseStrictDouble(mo, mov) || !parseStrictDouble(d, dv)) {
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
    if (!parseStrictDouble(hh, hhv) || !parseStrictDouble(mm, mmv)) {
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

  long long totalMinutes = daysFromCivil(year, mon, day) * 1440LL + static_cast<long long>(hour) * 60LL +
                           static_cast<long long>(minute);
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

std::string applyFormat(const std::string& rawText, const FormatSpec& fmt, bool numeric,
                        double numericValue) {
  std::string out = numeric ? std::string() : rawText;

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

bool parsePathSegment(std::string_view segment, std::string& key, std::vector<int>& indices) {
  key.clear();
  indices.clear();

  segment = trimView(segment);
  if (segment.empty()) {
    return false;
  }

  size_t i = 0;
  while (i < segment.size() && segment[i] != '[') {
    ++i;
  }
  key.assign(segment.substr(0, i));
  key = trimCopy(key);

  while (i < segment.size()) {
    if (segment[i] != '[') {
      return false;
    }
    ++i;
    const size_t idxStart = i;
    while (i < segment.size() && segment[i] != ']') {
      ++i;
    }
    if (i >= segment.size()) {
      return false;
    }

    std::string idxText(segment.substr(idxStart, i - idxStart));
    idxText = trimCopy(idxText);
    if (idxText.empty()) {
      return false;
    }
    for (char c : idxText) {
      if (c < '0' || c > '9') {
        return false;
      }
    }

    double idxValue = 0.0;
    if (!parseStrictDouble(idxText, idxValue)) {
      return false;
    }
    if (idxValue < 0 || idxValue > static_cast<double>(std::numeric_limits<int>::max())) {
      return false;
    }
    indices.push_back(static_cast<int>(idxValue));
    ++i;
  }

  return !key.empty() || !indices.empty();
}

bool resolveJsonPath(std::string_view rootText, const std::string& path, std::string_view& outValue) {
  std::string resolvedPath = bindRuntimeTemplate(path);
  std::string_view pathView = trimView(resolvedPath);
  if (pathView.empty()) {
    return false;
  }
  if (pathView.rfind("sort_num(", 0) == 0 || pathView.rfind("sort_alpha(", 0) == 0 ||
      pathView.rfind("distance_sort(", 0) == 0 || pathView.rfind("sort_distance(", 0) == 0) {
    ESP_LOGW(kTag, "sort transforms pending in IDF runtime path=%s", resolvedPath.c_str());
    return false;
  }

  std::string_view current = trimView(rootText);
  size_t start = 0;

  while (start <= pathView.size()) {
    size_t dot = pathView.find('.', start);
    if (dot == std::string_view::npos) {
      dot = pathView.size();
    }
    std::string_view segment = pathView.substr(start, dot - start);

    std::string key;
    std::vector<int> indices;
    if (!parsePathSegment(segment, key, indices)) {
      return false;
    }

    if (!key.empty()) {
      std::string_view next;
      if (!objectMemberValue(current, key, next)) {
        return false;
      }
      current = next;
    }

    for (int idx : indices) {
      std::string_view next;
      if (!arrayElementValue(current, idx, next)) {
        return false;
      }
      current = next;
    }

    if (dot >= pathView.size()) {
      break;
    }
    start = dot + 1;
  }

  outValue = trimView(current);
  return !outValue.empty();
}

std::string valueViewToText(std::string_view valueView) {
  valueView = trimView(valueView);
  if (valueView.empty()) {
    return {};
  }

  if (valueView.front() == '"') {
    std::string out;
    size_t next = 0;
    if (parseQuotedString(valueView, 0, out, next)) {
      return out;
    }
    return {};
  }

  if (valueView == "true") {
    return "true";
  }
  if (valueView == "false") {
    return "false";
  }
  if (valueView == "null") {
    return {};
  }

  std::string raw(valueView.data(), valueView.size());
  double parsed = 0.0;
  if (parseStrictDouble(raw, parsed)) {
    if (std::fabs(parsed - std::round(parsed)) < 0.000001) {
      return std::to_string(static_cast<long long>(std::llround(parsed)));
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.3f", parsed);
    std::string out = buf;
    while (!out.empty() && out.back() == '0') {
      out.pop_back();
    }
    if (!out.empty() && out.back() == '.') {
      out.pop_back();
    }
    return out;
  }

  return raw;
}

bool valueViewToNumeric(std::string_view valueView, double& out) {
  return viewToDouble(valueView, out);
}

void mapWeatherCode(int code, std::string& outText, std::string& outIcon) {
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
  } else if (code == 61 || code == 63 || code == 65 || code == 66 || code == 67 || code == 80 ||
             code == 81 || code == 82) {
    outText = "Rain";
    outIcon = "/icons/meteocons/rain.raw";
  } else if (code == 71 || code == 73 || code == 75 || code == 77 || code == 85 || code == 86) {
    outText = "Snow";
    outIcon = "/icons/meteocons/snow.raw";
  } else if (code == 95 || code == 96 || code == 99) {
    outText = "Storm";
    outIcon = "/icons/meteocons/thunderstorms-day.raw";
  }
}

bool computeMoonPhaseFraction(float& out) {
  const time_t nowUtc = std::time(nullptr);
  if (nowUtc < 946684800) {
    return false;
  }

  std::tm nowTm = {};
  gmtime_r(&nowUtc, &nowTm);

  const double daysNow =
      static_cast<double>(daysFromCivil(nowTm.tm_year + 1900, nowTm.tm_mon + 1, nowTm.tm_mday)) +
      (static_cast<double>(nowTm.tm_hour) + static_cast<double>(nowTm.tm_min) / 60.0 +
       static_cast<double>(nowTm.tm_sec) / 3600.0) /
          24.0;

  const double epochDays = static_cast<double>(daysFromCivil(2000, 1, 6)) + (18.0 + 14.0 / 60.0) / 24.0;
  const double synodic = 29.53058867;
  double age = std::fmod(daysNow - epochDays, synodic);
  if (age < 0) {
    age += synodic;
  }
  out = static_cast<float>(age / synodic);
  return true;
}

bool computeMoonPhaseName(std::string& out) {
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

bool parseFormat(std::string_view formatObj, FormatSpec& out) {
  int round = -1;
  if (objectMemberInt(formatObj, "round", round)) {
    out.roundDigits = round;
  } else if (objectMemberInt(formatObj, "round_digits", round)) {
    out.roundDigits = round;
  }

  (void)objectMemberString(formatObj, "unit", out.unit);
  (void)objectMemberString(formatObj, "locale", out.locale);
  (void)objectMemberString(formatObj, "prefix", out.prefix);
  (void)objectMemberString(formatObj, "suffix", out.suffix);
  (void)objectMemberString(formatObj, "tz", out.tz);

  if (!objectMemberString(formatObj, "time_format", out.timeFormat)) {
    (void)objectMemberString(formatObj, "timeFormat", out.timeFormat);
  }
  return true;
}

bool parseFieldSpec(const std::string& key, std::string_view valueText, FieldSpec& out) {
  valueText = trimView(valueText);
  if (valueText.empty()) {
    return false;
  }

  out = {};
  out.key = key;

  if (valueText.front() == '"') {
    if (!viewToString(valueText, out.path)) {
      return false;
    }
    return !out.path.empty();
  }

  if (valueText.front() != '{') {
    return false;
  }

  if (!objectMemberString(valueText, "path", out.path)) {
    return false;
  }

  std::string_view formatObj;
  if (objectMemberObject(valueText, "format", formatObj)) {
    (void)parseFormat(formatObj, out.format);
  }

  return !out.path.empty();
}

void parseLabelNodes(std::string_view nodesArray) {
  s.labels.clear();
  forEachArrayElement(nodesArray, [](int /*idx*/, std::string_view nodeValue) {
    nodeValue = trimView(nodeValue);
    if (nodeValue.empty() || nodeValue.front() != '{') {
      return;
    }

    std::string type;
    if (!objectMemberString(nodeValue, "type", type) || type != "label") {
      return;
    }

    LabelNode node = {};
    (void)objectMemberInt(nodeValue, "x", node.x);
    (void)objectMemberInt(nodeValue, "y", node.y);
    (void)objectMemberInt(nodeValue, "font", node.font);
    if (!objectMemberString(nodeValue, "text", node.text) || node.text.empty()) {
      return;
    }

    s.labels.push_back(std::move(node));
  });
}

bool loadDslConfig(const std::string& dslJson) {
  std::string_view root = trimView(dslJson);
  if (root.empty() || root.front() != '{') {
    return false;
  }

  std::string_view dataObj;
  if (!objectMemberObject(root, "data", dataObj)) {
    return false;
  }

  std::string source;
  if (!objectMemberString(dataObj, "source", source)) {
    source = "http";
  }
  std::transform(source.begin(), source.end(), source.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });

  if (source == "http") {
    s.source = DataSource::kHttp;
  } else if (source == "local_time") {
    s.source = DataSource::kLocalTime;
  } else {
    s.source = DataSource::kUnknown;
  }

  s.urlTemplate.clear();
  (void)objectMemberString(dataObj, "url", s.urlTemplate);

  int poll = static_cast<int>(kDefaultPollMs);
  if (objectMemberInt(dataObj, "poll_ms", poll) && poll > 0) {
    s.pollMs = static_cast<uint32_t>(poll);
  } else {
    s.pollMs = kDefaultPollMs;
  }

  bool debug = false;
  if (objectMemberBool(dataObj, "debug", debug)) {
    s.debug = debug;
  }

  s.fields.clear();
  s.values.clear();

  std::string_view fieldsObj;
  if (!objectMemberObject(dataObj, "fields", fieldsObj)) {
    return false;
  }

  forEachObjectMember(fieldsObj, [](const std::string& key, std::string_view valueText) {
    FieldSpec spec;
    if (!parseFieldSpec(key, valueText, spec)) {
      return;
    }
    s.fields.push_back(std::move(spec));
  });

  for (const FieldSpec& f : s.fields) {
    s.values.push_back({f.key, ""});
  }

  std::string_view uiObj;
  if (objectMemberObject(root, "ui", uiObj)) {
    std::string_view nodesArray;
    if (objectMemberArray(uiObj, "nodes", nodesArray)) {
      parseLabelNodes(nodesArray);
    }
  }

  if (s.source == DataSource::kHttp && s.urlTemplate.empty()) {
    ESP_LOGE(kTag, "dsl missing data.url for http source");
    return false;
  }

  return !s.fields.empty();
}

bool buildLocalTimeContext(LocalTimeContext& out) {
  const time_t nowUtc = std::time(nullptr);
  if (nowUtc < 946684800) {
    return false;
  }

  out = {};
  out.timezone = loadGeoTimezone();
  out.hasOffset = loadGeoOffsetMinutes(out.utcOffsetMinutes);
  if (!out.hasOffset) {
    out.hasOffset = inferOffsetFromTimezone(out.timezone, out.utcOffsetMinutes);
  }

  time_t localEpoch = nowUtc;
  if (out.hasOffset) {
    localEpoch += static_cast<time_t>(out.utcOffsetMinutes) * 60;
  }

  std::tm tmLocal = {};
  gmtime_r(&localEpoch, &tmLocal);

  out.year = tmLocal.tm_year + 1900;
  out.mon = tmLocal.tm_mon + 1;
  out.day = tmLocal.tm_mday;
  out.hour = tmLocal.tm_hour;
  out.minute = tmLocal.tm_min;
  out.second = tmLocal.tm_sec;

  const int safeYear = std::clamp(out.year, 0, 9999);
  const int safeMon = std::clamp(out.mon, 1, 12);
  const int safeDay = std::clamp(out.day, 1, 31);
  const int safeHour = std::clamp(out.hour, 0, 23);
  const int safeMinute = std::clamp(out.minute, 0, 59);
  const int safeSecond = std::clamp(out.second, 0, 59);

  char dateBuf[16];
  std::snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", safeYear, safeMon, safeDay);
  out.date = dateBuf;

  char time24Buf[16];
  std::snprintf(time24Buf, sizeof(time24Buf), "%02d:%02d:%02d", safeHour, safeMinute, safeSecond);
  out.time24 = time24Buf;

  int h12 = safeHour % 12;
  if (h12 == 0) {
    h12 = 12;
  }
  char time12Buf[16];
  std::snprintf(time12Buf, sizeof(time12Buf), "%02d:%02d:%02d %s", h12, safeMinute, safeSecond,
                safeHour >= 12 ? "PM" : "AM");
  out.time12 = time12Buf;

  char isoBuf[24];
  std::snprintf(isoBuf, sizeof(isoBuf), "%04d-%02d-%02dT%02d:%02d", safeYear, safeMon, safeDay,
                safeHour, safeMinute);
  out.isoLocal = isoBuf;

  return true;
}

bool resolveLocalTimeValue(const LocalTimeContext& ctx, const std::string& path, std::string& raw,
                           bool& numeric, double& numericValue) {
  numeric = false;
  numericValue = 0.0;

  if (path == "hour") {
    numeric = true;
    numericValue = static_cast<double>(ctx.hour);
    raw = std::to_string(ctx.hour);
    return true;
  }
  if (path == "minute") {
    numeric = true;
    numericValue = static_cast<double>(ctx.minute);
    raw = std::to_string(ctx.minute);
    return true;
  }
  if (path == "second") {
    numeric = true;
    numericValue = static_cast<double>(ctx.second);
    raw = std::to_string(ctx.second);
    return true;
  }
  if (path == "date") {
    raw = ctx.date;
    return true;
  }
  if (path == "iso_local") {
    raw = ctx.isoLocal;
    return true;
  }
  if (path == "time") {
    raw = RuntimeSettings::use24HourClock ? ctx.time24 : ctx.time12;
    return true;
  }
  if (path == "time_24") {
    raw = ctx.time24;
    return true;
  }
  if (path == "time_12") {
    raw = ctx.time12;
    return true;
  }
  if (path == "offset_min") {
    numeric = true;
    numericValue = static_cast<double>(ctx.utcOffsetMinutes);
    raw = std::to_string(ctx.utcOffsetMinutes);
    return true;
  }
  if (path == "offset_known") {
    raw = ctx.hasOffset ? "true" : "false";
    return true;
  }
  if (path == "tz") {
    raw = ctx.timezone;
    return true;
  }

  return false;
}

void applyWeatherDerivedValues() {
  const auto applyWeather = [](const char* codeKey, const char* textKey, const char* iconKey) {
    const std::string* codeText = getValue(codeKey);
    if (codeText == nullptr || codeText->empty()) {
      (void)setValue(textKey, "");
      (void)setValue(iconKey, "");
      return;
    }

    double codeValue = 0.0;
    if (!parseStrictDouble(*codeText, codeValue)) {
      return;
    }

    std::string text;
    std::string icon;
    mapWeatherCode(static_cast<int>(std::lround(codeValue)), text, icon);
    (void)setValue(textKey, text);
    (void)setValue(iconKey, icon);
  };

  applyWeather("code_now", "cond_now", "icon_now");
  applyWeather("day1_code", "day1_cond", "day1_icon");
  applyWeather("day2_code", "day2_cond", "day2_icon");
}

esp_err_t httpEventHandler(esp_http_client_event_t* evt) {
  if (evt == nullptr || evt->user_data == nullptr) {
    return ESP_OK;
  }
  HttpCapture* cap = static_cast<HttpCapture*>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data != nullptr && evt->data_len > 0) {
    cap->body.append(static_cast<const char*>(evt->data), static_cast<size_t>(evt->data_len));
  }
  return ESP_OK;
}

bool httpGet(const std::string& url, int& statusCode, std::string& body, std::string& reason) {
  statusCode = 0;
  body.clear();
  reason.clear();

  if (sHttpGate == nullptr) {
    sHttpGate = xSemaphoreCreateMutex();
    if (sHttpGate == nullptr) {
      reason = "http gate alloc failed";
      return false;
    }
  }

  if (xSemaphoreTake(sHttpGate, pdMS_TO_TICKS(kHttpGateTimeoutMs)) != pdTRUE) {
    reason = "transport gate timeout";
    return false;
  }

  HttpCapture cap;
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.timeout_ms = static_cast<int>(kHttpTimeoutMs);
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.disable_auto_redirect = false;
  cfg.max_redirection_count = 5;
  cfg.keep_alive_enable = false;
  cfg.event_handler = httpEventHandler;
  cfg.user_data = &cap;
  cfg.buffer_size = 1024;
  cfg.buffer_size_tx = 512;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) {
    xSemaphoreGive(sHttpGate);
    reason = "http init failed";
    return false;
  }

  esp_http_client_set_method(client, HTTP_METHOD_GET);
  esp_http_client_set_header(client, "Accept", "application/json");
  esp_http_client_set_header(client, "User-Agent", "CoStar-ESP32/1.0");
  esp_http_client_set_header(client, "Accept-Encoding", "identity");

  const esp_err_t err = esp_http_client_perform(client);
  if (err == ESP_OK) {
    statusCode = esp_http_client_get_status_code(client);
  }
  body = cap.body;

  esp_http_client_cleanup(client);
  xSemaphoreGive(sHttpGate);

  if (err != ESP_OK) {
    reason = esp_err_to_name(err);
    return false;
  }
  if (statusCode <= 0) {
    reason = "no-http-status";
    return false;
  }

  ESP_LOGI(kTag, "http done status=%d bytes=%u", statusCode, static_cast<unsigned>(body.size()));
  return true;
}

void noteFetchFailure(uint32_t nowMs, const char* reason) {
  if (s.failureStreak < 255) {
    ++s.failureStreak;
  }
  const uint8_t shift = std::min<uint8_t>(s.failureStreak, 5);
  const uint32_t delayMs = std::min<uint32_t>(60000U, 2000U << shift);
  s.backoffUntilMs = nowMs + delayMs;
  ESP_LOGW(kTag, "fetch fail widget=%s streak=%u backoff_ms=%u reason=%s", s.widgetId.c_str(),
           static_cast<unsigned>(s.failureStreak), static_cast<unsigned>(delayMs),
           reason != nullptr ? reason : "unknown");
}

void noteFetchSuccess() {
  s.failureStreak = 0;
  s.backoffUntilMs = 0;
}

bool resolveFieldsFromHttp(std::string_view jsonText) {
  int resolved = 0;
  int missing = 0;

  for (const FieldSpec& field : s.fields) {
    std::string path = bindRuntimeTemplate(field.path);
    std::string raw;
    bool numeric = false;
    double numericValue = 0.0;

    if (path == "computed.moon_phase") {
      if (!computeMoonPhaseName(raw)) {
        ++missing;
        (void)setValue(field.key, "");
        continue;
      }
    } else {
      std::string_view valueView;
      if (!resolveJsonPath(jsonText, path, valueView)) {
        ++missing;
        if (s.debug) {
          ESP_LOGW(kTag, "field miss key=%s path=%s", field.key.c_str(), path.c_str());
        }
        (void)setValue(field.key, "");
        continue;
      }

      raw = valueViewToText(valueView);
      numeric = valueViewToNumeric(valueView, numericValue);
    }

    FormatSpec resolvedFmt = field.format;
    resolvedFmt.unit = bindRuntimeTemplate(resolvedFmt.unit);
    resolvedFmt.locale = bindRuntimeTemplate(resolvedFmt.locale);
    resolvedFmt.prefix = bindRuntimeTemplate(resolvedFmt.prefix);
    resolvedFmt.suffix = bindRuntimeTemplate(resolvedFmt.suffix);
    resolvedFmt.tz = bindRuntimeTemplate(resolvedFmt.tz);
    resolvedFmt.timeFormat = bindRuntimeTemplate(resolvedFmt.timeFormat);

    const std::string formatted = applyFormat(raw, resolvedFmt, numeric, numericValue);
    (void)setValue(field.key, formatted);
    ++resolved;
  }

  applyWeatherDerivedValues();

  if (s.debug) {
    ESP_LOGI(kTag, "parse summary resolved=%d missing=%d total=%u", resolved, missing,
             static_cast<unsigned>(s.fields.size()));
  }

  return resolved > 0;
}

bool resolveFieldsFromLocalTime() {
  LocalTimeContext ctx;
  if (!buildLocalTimeContext(ctx)) {
    return false;
  }

  int resolved = 0;
  int missing = 0;
  for (const FieldSpec& field : s.fields) {
    const std::string path = bindRuntimeTemplate(field.path);

    std::string raw;
    bool numeric = false;
    double numericValue = 0.0;
    if (!resolveLocalTimeValue(ctx, path, raw, numeric, numericValue)) {
      ++missing;
      (void)setValue(field.key, "");
      continue;
    }

    FormatSpec resolvedFmt = field.format;
    resolvedFmt.unit = bindRuntimeTemplate(resolvedFmt.unit);
    resolvedFmt.locale = bindRuntimeTemplate(resolvedFmt.locale);
    resolvedFmt.prefix = bindRuntimeTemplate(resolvedFmt.prefix);
    resolvedFmt.suffix = bindRuntimeTemplate(resolvedFmt.suffix);
    resolvedFmt.tz = bindRuntimeTemplate(resolvedFmt.tz);
    resolvedFmt.timeFormat = bindRuntimeTemplate(resolvedFmt.timeFormat);

    const std::string formatted = applyFormat(raw, resolvedFmt, numeric, numericValue);
    (void)setValue(field.key, formatted);
    ++resolved;
  }

  if (s.debug) {
    ESP_LOGI(kTag, "local_time summary resolved=%d missing=%d total=%u", resolved, missing,
             static_cast<unsigned>(s.fields.size()));
  }

  return resolved > 0;
}

bool fetchAndResolve(uint32_t nowMs) {
  if (s.source == DataSource::kLocalTime) {
    if (!resolveFieldsFromLocalTime()) {
      noteFetchFailure(nowMs, "local_time unavailable");
      return false;
    }
    noteFetchSuccess();
    return true;
  }

  if (s.source != DataSource::kHttp) {
    noteFetchFailure(nowMs, "unsupported source");
    return false;
  }

  const std::string url = bindRuntimeTemplate(s.urlTemplate);
  int statusCode = 0;
  std::string body;
  std::string reason;
  if (!httpGet(url, statusCode, body, reason)) {
    noteFetchFailure(nowMs, reason.c_str());
    return false;
  }
  if (statusCode < 200 || statusCode >= 300) {
    char statusBuf[32];
    std::snprintf(statusBuf, sizeof(statusBuf), "status=%d", statusCode);
    noteFetchFailure(nowMs, statusBuf);
    return false;
  }

  if (body.empty()) {
    noteFetchFailure(nowMs, "empty body");
    return false;
  }

  if (!resolveFieldsFromHttp(body)) {
    noteFetchFailure(nowMs, "dsl parse unresolved");
    return false;
  }

  noteFetchSuccess();
  return true;
}

void drawGlyph(int x, int y, char ch, uint16_t fg, uint16_t bg, int scale) {
  const uint8_t c = static_cast<uint8_t>(ch);
  const size_t base = static_cast<size_t>(c) * 5U;
  for (int col = 0; col < 5; ++col) {
    const uint8_t bits = font[base + static_cast<size_t>(col)];
    for (int row = 0; row < 7; ++row) {
      const uint16_t px = static_cast<uint16_t>(x + col * scale);
      const uint16_t py = static_cast<uint16_t>(y + row * scale);
      const bool on = ((bits >> row) & 1U) != 0;
      (void)display_spi::fillRect(px, py, static_cast<uint16_t>(scale), static_cast<uint16_t>(scale),
                                  on ? fg : bg);
    }
  }
}

void drawText(int x, int y, const std::string& text, uint16_t fg, uint16_t bg, int scale) {
  int penX = x;
  for (char c : text) {
    drawGlyph(penX, y, c, fg, bg, scale);
    penX += 6 * scale;
  }
}

void render() {
  if (!s.active) {
    return;
  }

  (void)display_spi::fillRect(s.x, s.y, s.w, s.h, kBg);
  (void)display_spi::fillRect(s.x, s.y, s.w, 1, kBorder);
  (void)display_spi::fillRect(s.x, static_cast<uint16_t>(s.y + s.h - 1), s.w, 1, kBorder);
  (void)display_spi::fillRect(s.x, s.y, 1, s.h, kBorder);
  (void)display_spi::fillRect(static_cast<uint16_t>(s.x + s.w - 1), s.y, 1, s.h, kBorder);

  drawText(static_cast<int>(s.x) + 4, static_cast<int>(s.y) + 4, s.widgetId, kAccent, kBg, 1);

  if (!s.hasData) {
    drawText(static_cast<int>(s.x) + 6, static_cast<int>(s.y) + 22, "LOADING...", kText, kBg, 1);
    return;
  }

  for (const LabelNode& node : s.labels) {
    const std::string line = bindRuntimeTemplate(node.text);
    const int scale = std::max(1, std::min(3, node.font <= 1 ? 1 : (node.font >= 4 ? 2 : 1)));
    drawText(static_cast<int>(s.x) + node.x, static_cast<int>(s.y) + node.y, line, kText, kBg, scale);
  }
}

}  // namespace

namespace dsl_widget_runtime {

void reset() { s = {}; }

bool begin(const char* widgetId, const char* dslPath, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  reset();
  s.active = true;
  s.widgetId = (widgetId != nullptr) ? widgetId : "dsl";
  s.dslPath = (dslPath != nullptr) ? dslPath : "";
  s.x = x;
  s.y = y;
  s.w = w;
  s.h = h;

  const std::string dslJson = readFile(s.dslPath.c_str());
  if (dslJson.empty() || !loadDslConfig(dslJson)) {
    ESP_LOGE(kTag, "dsl load failed widget=%s path=%s", s.widgetId.c_str(), s.dslPath.c_str());
    s.active = false;
    return false;
  }

  ESP_LOGI(kTag, "begin widget=%s path=%s source=%d poll_ms=%u fields=%u labels=%u",
           s.widgetId.c_str(), s.dslPath.c_str(), static_cast<int>(s.source),
           static_cast<unsigned>(s.pollMs), static_cast<unsigned>(s.fields.size()),
           static_cast<unsigned>(s.labels.size()));

  render();
  return true;
}

void tick(uint32_t nowMs) {
  if (!s.active) {
    return;
  }

  if (s.backoffUntilMs != 0 && static_cast<int32_t>(nowMs - s.backoffUntilMs) < 0) {
    return;
  }

  const uint32_t cadence = s.hasData ? s.pollMs : kInitialPollMs;
  if (s.lastFetchMs == 0 || (nowMs - s.lastFetchMs) >= cadence) {
    s.lastFetchMs = nowMs;
    if (fetchAndResolve(nowMs)) {
      s.hasData = true;
      ESP_LOGI(kTag, "update ok widget=%s", s.widgetId.c_str());
    }
    render();
  }
}

bool isActive() { return s.active; }

}  // namespace dsl_widget_runtime
