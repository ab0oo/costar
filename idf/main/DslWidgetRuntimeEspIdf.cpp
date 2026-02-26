#include "DslWidgetRuntimeEspIdf.h"

#include "AppConfig.h"
#include "DisplaySpiEspIdf.h"
#include "Font5x7Classic.h"
#include "RuntimeSettings.h"
#include "platform/Platform.h"
#include "platform/Prefs.h"

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

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
#include <map>
#include <memory>
#include <sys/stat.h>
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
constexpr uint32_t kHttpWorkerQueueLen = 4U;
constexpr uint32_t kHttpWorkerReplyTimeoutMs = 9000U;
constexpr uint32_t kHttpWorkerStack = 8192U;
constexpr uint32_t kHttpResponseMaxBytesDefault = 16384U;
constexpr uint32_t kHttpResponseMaxBytesMin = 1024U;
constexpr uint32_t kHttpResponseMaxBytesMax = 32768U;
constexpr UBaseType_t kHttpWorkerPriority = 4U;
constexpr BaseType_t kHttpWorkerCore = 0;
constexpr uint32_t kHaWsConnectTimeoutMs = 15000U;
constexpr uint32_t kHaWsDefaultKeepAliveMs = 30000U;
constexpr size_t kHaWsMaxFrameBytes = 16384U;
constexpr size_t kHaWsDiagLargeFrameBytes = 3000U;
constexpr uint32_t kTapPostHttpRefreshDelayMs = 750U;
constexpr size_t kIconMemCacheBudgetBytes = 192U * 1024U;
constexpr const char* kIconCacheDir = "/littlefs/icon_cache";
constexpr uint32_t kIconFetchRetryMs = 30000U;
constexpr size_t kUiCriticalLargest8Bit = 12288U;
constexpr size_t kUiCriticalFree8Bit = 24576U;

enum class DataSource : uint8_t {
  kHttp,
  kHaWs,
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

enum class NodeType : uint8_t {
  kLabel,
  kValueBox,
  kProgress,
  kSparkline,
  kIcon,
  kMoonPhase,
  kArc,
  kLine,
};

enum class OverflowMode : uint8_t {
  kClip,
  kEllipsis,
};

enum class HAlign : uint8_t {
  kLeft,
  kCenter,
  kRight,
};

enum class VAlign : uint8_t {
  kTop,
  kCenter,
  kBottom,
};

enum class TextDatum : uint8_t {
  kTL,
  kTC,
  kTR,
  kML,
  kMC,
  kMR,
  kBL,
  kBC,
  kBR,
  kLBaseline,
  kCBaseline,
  kRBaseline,
};

struct Node {
  NodeType type = NodeType::kLabel;
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  int x2 = 0;
  int y2 = 0;
  int font = 1;
  uint16_t color565 = 0xFFFF;
  uint16_t bg565 = 0x0000;
  std::string text;
  std::string key;
  std::string path;
  std::string angleExpr;
  bool wrap = false;
  int lineHeight = 0;
  int maxLines = 0;
  OverflowMode overflow = OverflowMode::kClip;
  HAlign align = HAlign::kLeft;
  VAlign valign = VAlign::kTop;
  TextDatum datum = TextDatum::kTL;
  float min = 0.0f;
  float max = 100.0f;
  float startDeg = 0.0f;
  float endDeg = 360.0f;
  int radius = 0;
  int length = 0;
  int thickness = 1;
  std::vector<float> sparkValues;
};

struct ModalSpec {
  std::string id;
  std::string title;
  std::string text;
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  int font = 1;
  int lineHeight = 0;
  int maxLines = 0;
  uint16_t bg565 = 0x0000;
  uint16_t border565 = 0xFFFF;
  uint16_t titleColor565 = 0xFFFF;
  uint16_t textColor565 = 0xFFFF;
};

enum class TouchActionType : uint8_t {
  kNone,
  kModal,
  kHttp,
};

struct TouchRegion {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  TouchActionType action = TouchActionType::kNone;
  std::string modalId;
  uint32_t dismissMs = 0;
  std::string httpUrl;
  std::string httpMethod = "POST";
  std::string httpBody;
  std::string httpContentType = "application/json";
  std::vector<std::pair<std::string, std::string>> httpHeaders;
};

struct KeyValue {
  std::string key;
  std::string value;
};

struct HttpCapture {
  std::string body;
  size_t maxBytes = kHttpResponseMaxBytesDefault;
  bool overflow = false;
};

struct HttpJob {
  std::string method;
  std::string url;
  std::string body;
  std::vector<KeyValue> headers;
  uint32_t maxResponseBytes = kHttpResponseMaxBytesDefault;
  QueueHandle_t replyQueue = nullptr;
};

struct HttpResult {
  bool ok = false;
  int statusCode = 0;
  std::string body;
  std::string reason;
  uint32_t durationMs = 0;
  std::string host;
  bool viaProxy = false;
};

enum class TapActionType : uint8_t {
  kNone,
  kRefresh,
  kHttp,
  kHaWsService,
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
  TapActionType tapAction = TapActionType::kNone;
  std::string tapUrlTemplate;
  std::string tapMethod;
  std::string tapBodyTemplate;
  std::string tapContentType;
  std::vector<KeyValue> tapHeaders;
  std::string urlTemplate;
  std::string wsEntityTemplate;
  std::vector<KeyValue> headers;
  std::vector<std::string> transforms;
  std::vector<FieldSpec> fields;
  std::vector<Node> nodes;
  std::vector<ModalSpec> modals;
  std::vector<TouchRegion> touchRegions;
  std::vector<KeyValue> values;
  std::map<std::string, double> numericValues;
  std::map<std::string, std::string> transformValues;
  std::map<std::string, std::string> settingValues;
  std::string activeModalId;
  uint32_t modalDismissDueMs = 0;
  std::string sourceJson;
  bool retainSourceJson = false;
  std::string transformJson;
  uint32_t tapRefreshDueMs = 0;
  uint32_t httpMaxBytes = kHttpResponseMaxBytesDefault;
};

State s;
std::vector<State> sInstances;
SemaphoreHandle_t sHttpGate = nullptr;
QueueHandle_t sHttpJobQueue = nullptr;
TaskHandle_t sHttpWorkerTask = nullptr;
uint16_t* sCanvas = nullptr;
uint16_t sCanvasW = 0;
uint16_t sCanvasH = 0;
uint16_t sCanvasY0 = 0;

struct IconMemEntry {
  std::vector<uint16_t> pixels;
  uint32_t lastUsedMs = 0;
};

std::map<std::string, IconMemEntry> sIconMemCache;
size_t sIconMemCacheBytes = 0;
bool sIconCacheDirReady = false;
std::map<std::string, uint32_t> sIconRetryAfterMs;

struct HaWsState {
  SemaphoreHandle_t lock = nullptr;
  esp_websocket_client_handle_t client = nullptr;
  std::string wsUrl;
  std::string token;
  bool authOk = false;
  bool ready = false;
  bool started = false;
  uint32_t nextReqId = 1;
  uint32_t reconnectDueMs = 0;
  uint8_t failureStreak = 0;
  std::string rxFrame;
  std::map<std::string, std::string> entityStateJson;
  std::map<uint32_t, std::string> renderReqToEntityId;
  std::map<std::string, uint32_t> entityIdToRenderReq;
  std::map<uint32_t, std::string> triggerReqToEntityId;
  std::map<std::string, uint32_t> entityIdToTriggerReq;
  std::map<uint32_t, std::string> triggerSubIdToEntityId;
  std::map<std::string, uint32_t> entityIdToTriggerSubId;
};

HaWsState sHaWs;

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

uint16_t rgbTo565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8U) << 8U) | ((g & 0xFCU) << 3U) | (b >> 3U));
}

bool parseHexColor565(const std::string& hex, uint16_t& outColor) {
  if (hex.size() != 7 || hex[0] != '#') {
    return false;
  }
  char* endPtr = nullptr;
  const long value = std::strtol(hex.c_str() + 1, &endPtr, 16);
  if (endPtr == nullptr || *endPtr != '\0' || value < 0 || value > 0xFFFFFF) {
    return false;
  }
  outColor =
      rgbTo565(static_cast<uint8_t>((value >> 16) & 0xFF), static_cast<uint8_t>((value >> 8) & 0xFF),
               static_cast<uint8_t>(value & 0xFF));
  return true;
}

struct VarContext {
  const VarContext* parent = nullptr;
  std::string name;
  float value = 0.0f;
};

const std::string* getValue(const std::string& key);

bool lookupVar(const VarContext* ctx, const std::string& name, float& out) {
  for (const VarContext* cur = ctx; cur != nullptr; cur = cur->parent) {
    if (cur->name == name) {
      out = cur->value;
      return true;
    }
  }
  return false;
}

std::string formatVarValue(float value) {
  if (std::fabs(value - std::round(value)) < 0.0001f) {
    return std::to_string(static_cast<int>(std::lround(value)));
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(value));
  std::string out = buf;
  while (!out.empty() && out.back() == '0') {
    out.pop_back();
  }
  if (!out.empty() && out.back() == '.') {
    out.pop_back();
  }
  return out;
}

std::string substituteTemplateVars(const std::string& input, const VarContext* ctx) {
  if (ctx == nullptr || input.find("{{") == std::string::npos) {
    return input;
  }
  std::string out;
  out.reserve(input.size());
  size_t pos = 0;
  while (pos < input.size()) {
    const size_t start = input.find("{{", pos);
    if (start == std::string::npos) {
      out.append(input, pos, std::string::npos);
      break;
    }
    out.append(input, pos, start - pos);
    const size_t end = input.find("}}", start + 2);
    if (end == std::string::npos) {
      out.append(input, start, std::string::npos);
      break;
    }
    std::string key(input.substr(start + 2, end - (start + 2)));
    const auto notWs = [](unsigned char c) { return c != ' ' && c != '\n' && c != '\r' && c != '\t'; };
    key.erase(key.begin(), std::find_if(key.begin(), key.end(), notWs));
    key.erase(std::find_if(key.rbegin(), key.rend(), notWs).base(), key.end());
    float value = 0.0f;
    if (lookupVar(ctx, key, value)) {
      out += formatVarValue(value);
    } else {
      out.append(input, start, end - start + 2);
    }
    pos = end + 2;
  }
  return out;
}

class ExprParser {
 public:
  ExprParser(const std::string& text, const VarContext* vars) : text_(text), vars_(vars) {}

  bool parse(float& out) {
    pos_ = 0;
    if (!parseExpr(out)) {
      return false;
    }
    skipWsLocal();
    return pos_ == text_.size();
  }

 private:
  const std::string& text_;
  size_t pos_ = 0;
  const VarContext* vars_ = nullptr;

  void skipWsLocal() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c != ' ' && c != '\t') {
        break;
      }
      ++pos_;
    }
  }

  bool parseIdent(std::string& out) {
    if (pos_ >= text_.size()) {
      return false;
    }
    const char c = text_[pos_];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) {
      return false;
    }
    const size_t start = pos_;
    while (pos_ < text_.size()) {
      const char ch = text_[pos_];
      if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '_')) {
        break;
      }
      ++pos_;
    }
    out.assign(text_.substr(start, pos_ - start));
    return true;
  }

  bool resolveVariable(const std::string& name, float& out) const {
    if (name == "pi") {
      out = static_cast<float>(M_PI);
      return true;
    }
    if (lookupVar(vars_, name, out)) {
      return true;
    }
    const auto it = s.numericValues.find(name);
    if (it != s.numericValues.end()) {
      out = static_cast<float>(it->second);
      return true;
    }
    if (const std::string* text = getValue(name); text != nullptr) {
      double parsed = 0.0;
      if (parseStrictDouble(*text, parsed)) {
        out = static_cast<float>(parsed);
        return true;
      }
    }
    return false;
  }

  bool parseFunction(const std::string& name, float& out) {
    skipWsLocal();
    if (pos_ >= text_.size() || text_[pos_] != '(') {
      return false;
    }
    ++pos_;
    skipWsLocal();

    float args[4] = {0, 0, 0, 0};
    int argc = 0;
    if (pos_ < text_.size() && text_[pos_] == ')') {
      ++pos_;
    } else {
      while (true) {
        if (argc >= 4 || !parseExpr(args[argc])) {
          return false;
        }
        ++argc;
        skipWsLocal();
        if (pos_ < text_.size() && text_[pos_] == ',') {
          ++pos_;
          skipWsLocal();
          continue;
        }
        if (pos_ >= text_.size() || text_[pos_] != ')') {
          return false;
        }
        ++pos_;
        break;
      }
    }

    const float degToRad = static_cast<float>(M_PI / 180.0);
    const float a = args[0];
    const float b = args[1];
    if (name == "sin" && argc == 1) {
      out = std::sin(a * degToRad);
      return true;
    }
    if (name == "cos" && argc == 1) {
      out = std::cos(a * degToRad);
      return true;
    }
    if (name == "tan" && argc == 1) {
      out = std::tan(a * degToRad);
      return true;
    }
    if (name == "asin" && argc == 1) {
      out = std::asin(a) / degToRad;
      return true;
    }
    if (name == "acos" && argc == 1) {
      out = std::acos(a) / degToRad;
      return true;
    }
    if (name == "atan" && argc == 1) {
      out = std::atan(a) / degToRad;
      return true;
    }
    if (name == "abs" && argc == 1) {
      out = std::fabs(a);
      return true;
    }
    if (name == "sqrt" && argc == 1 && a >= 0.0f) {
      out = std::sqrt(a);
      return true;
    }
    if (name == "floor" && argc == 1) {
      out = std::floor(a);
      return true;
    }
    if (name == "ceil" && argc == 1) {
      out = std::ceil(a);
      return true;
    }
    if (name == "round" && argc == 1) {
      out = std::round(a);
      return true;
    }
    if (name == "min" && argc == 2) {
      out = std::fmin(a, b);
      return true;
    }
    if (name == "max" && argc == 2) {
      out = std::fmax(a, b);
      return true;
    }
    if (name == "pow" && argc == 2) {
      out = std::pow(a, b);
      return true;
    }
    if (name == "rad" && argc == 1) {
      out = a * degToRad;
      return true;
    }
    if (name == "deg" && argc == 1) {
      out = a / degToRad;
      return true;
    }
    return false;
  }

  bool parseFactor(float& out) {
    skipWsLocal();
    if (pos_ >= text_.size()) {
      return false;
    }
    if (text_[pos_] == '(') {
      ++pos_;
      if (!parseExpr(out)) {
        return false;
      }
      skipWsLocal();
      if (pos_ >= text_.size() || text_[pos_] != ')') {
        return false;
      }
      ++pos_;
      return true;
    }
    if (text_[pos_] == '+' || text_[pos_] == '-') {
      const char sign = text_[pos_++];
      if (!parseFactor(out)) {
        return false;
      }
      if (sign == '-') {
        out = -out;
      }
      return true;
    }
    if ((text_[pos_] >= '0' && text_[pos_] <= '9') || text_[pos_] == '.') {
      const size_t start = pos_;
      while (pos_ < text_.size() &&
             ((text_[pos_] >= '0' && text_[pos_] <= '9') || text_[pos_] == '.')) {
        ++pos_;
      }
      double d = 0.0;
      if (!parseStrictDouble(text_.substr(start, pos_ - start), d)) {
        return false;
      }
      out = static_cast<float>(d);
      return true;
    }
    std::string ident;
    if (parseIdent(ident)) {
      skipWsLocal();
      if (pos_ < text_.size() && text_[pos_] == '(') {
        return parseFunction(ident, out);
      }
      return resolveVariable(ident, out);
    }
    return false;
  }

  bool parseTerm(float& out) {
    if (!parseFactor(out)) {
      return false;
    }
    while (true) {
      skipWsLocal();
      if (pos_ >= text_.size() || (text_[pos_] != '*' && text_[pos_] != '/' && text_[pos_] != '%')) {
        break;
      }
      const char op = text_[pos_++];
      float rhs = 0.0f;
      if (!parseFactor(rhs)) {
        return false;
      }
      if (op == '*') {
        out *= rhs;
      } else if (op == '/') {
        if (std::fabs(rhs) < 0.000001f) {
          return false;
        }
        out /= rhs;
      } else {
        if (std::fabs(rhs) < 0.000001f) {
          return false;
        }
        out = std::fmod(out, rhs);
      }
    }
    return true;
  }

  bool parseExpr(float& out) {
    if (!parseTerm(out)) {
      return false;
    }
    while (true) {
      skipWsLocal();
      if (pos_ >= text_.size() || (text_[pos_] != '+' && text_[pos_] != '-')) {
        break;
      }
      const char op = text_[pos_++];
      float rhs = 0.0f;
      if (!parseTerm(rhs)) {
        return false;
      }
      out = (op == '+') ? (out + rhs) : (out - rhs);
    }
    return true;
  }
};

bool evalNumericExpr(const std::string& input, const VarContext* ctx, float& out) {
  const std::string text = substituteTemplateVars(input, ctx);
  ExprParser parser(text, ctx);
  if (parser.parse(out)) {
    return true;
  }
  double d = 0.0;
  if (!parseStrictDouble(text, d)) {
    return false;
  }
  out = static_cast<float>(d);
  return true;
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

  if (key.rfind("setting.", 0) == 0) {
    const std::string settingKey = key.substr(8);
    auto it = s.settingValues.find(settingKey);
    if (it != s.settingValues.end()) {
      return it->second;
    }
    std::string pref = platform::prefs::getString("settings", settingKey.c_str(), "");
    if (!pref.empty()) {
      return pref;
    }
    pref = platform::prefs::getString("ha", settingKey.c_str(), "");
    if (!pref.empty()) {
      return pref;
    }
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

std::string valueViewToText(std::string_view valueView);
bool httpGet(const std::string& url, const std::vector<KeyValue>& headers, int& statusCode, std::string& body,
             std::string& reason);

float distanceKm(float lat1, float lon1, float lat2, float lon2) {
  constexpr float kDegToRad = 3.14159265f / 180.0f;
  constexpr float kEarthRadiusKm = 6371.0f;
  const float dLat = (lat2 - lat1) * kDegToRad;
  const float dLon = (lon2 - lon1) * kDegToRad;
  const float a = std::sin(dLat * 0.5f) * std::sin(dLat * 0.5f) +
                  std::cos(lat1 * kDegToRad) * std::cos(lat2 * kDegToRad) *
                      std::sin(dLon * 0.5f) * std::sin(dLon * 0.5f);
  const float c = 2.0f * std::atan2(std::sqrt(a), std::sqrt(1.0f - a));
  return kEarthRadiusKm * c;
}

bool resolveJsonPath(std::string_view rootText, const std::string& path, std::string_view& outValue);

bool resolveSortPath(std::string_view rootText, std::string_view pathView, std::string_view& outValue) {
  bool numericSort = false;
  bool distanceSort = false;
  size_t argsStart = 0;
  if (pathView.rfind("sort_num(", 0) == 0) {
    numericSort = true;
    argsStart = 9;
  } else if (pathView.rfind("sort_alpha(", 0) == 0) {
    argsStart = 11;
  } else if (pathView.rfind("distance_sort(", 0) == 0 || pathView.rfind("sort_distance(", 0) == 0) {
    distanceSort = true;
    argsStart = 14;
  } else {
    return false;
  }

  const size_t close = pathView.find(')', argsStart);
  if (close == std::string_view::npos) {
    return false;
  }

  std::vector<std::string> args;
  splitArgs(std::string(pathView.substr(argsStart, close - argsStart)), args);
  if (args.empty()) {
    return false;
  }
  const std::string arrayPath = args[0];
  if (arrayPath.empty()) {
    return false;
  }

  std::string keyPath;
  float originLat = 0.0f;
  float originLon = 0.0f;
  std::string order = "asc";

  if (distanceSort) {
    if (args.size() < 3 || args.size() > 4) {
      return false;
    }
    auto parseNumberArg = [&](const std::string& arg, float& outNum) -> bool {
      std::string trimmed = trimCopy(arg);
      double d = 0.0;
      if (parseStrictDouble(trimmed, d)) {
        outNum = static_cast<float>(d);
        return true;
      }
      std::string_view resolved;
      if (!resolveJsonPath(rootText, trimmed, resolved)) {
        return false;
      }
      double value = 0.0;
      if (!viewToDouble(resolved, value)) {
        return false;
      }
      outNum = static_cast<float>(value);
      return true;
    };
    if (!parseNumberArg(args[1], originLat) || !parseNumberArg(args[2], originLon)) {
      return false;
    }
    if (args.size() > 3) {
      order = args[3];
    }
  } else {
    if (args.size() < 2 || args.size() > 3) {
      return false;
    }
    keyPath = trimCopy(args[1]);
    if (args.size() > 2) {
      order = args[2];
    }
  }

  std::transform(order.begin(), order.end(), order.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  const bool descending = (order == "desc" || order == "reverse" || order == "rev");

  std::string tail = trimCopy(std::string(pathView.substr(close + 1)));
  if (!tail.empty() && tail.front() == '.') {
    tail.erase(tail.begin());
  }

  std::string_view arrayView;
  if (!resolveJsonPath(rootText, arrayPath, arrayView)) {
    return false;
  }
  arrayView = trimView(arrayView);
  if (arrayView.size() < 2 || arrayView.front() != '[' || arrayView.back() != ']') {
    return false;
  }

  std::vector<std::string_view> items;
  forEachArrayElement(arrayView, [&](int, std::string_view elem) { items.push_back(trimView(elem)); });
  if (items.empty()) {
    return false;
  }
  std::vector<int> indexOrder(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    indexOrder[i] = static_cast<int>(i);
  }

  auto numericOf = [&](std::string_view valueView, float& outNum) -> bool {
    double d = 0.0;
    if (viewToDouble(valueView, d)) {
      outNum = static_cast<float>(d);
      return true;
    }
    std::string text = valueViewToText(valueView);
    std::string filtered;
    filtered.reserve(text.size());
    bool hasDigit = false;
    for (char c : text) {
      if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+') {
        filtered.push_back(c);
        if (c >= '0' && c <= '9') {
          hasDigit = true;
        }
      }
    }
    double parsed = 0.0;
    if (!hasDigit || !parseStrictDouble(filtered, parsed)) {
      return false;
    }
    outNum = static_cast<float>(parsed);
    return true;
  };

  auto cmpAsc = [&](int lhs, int rhs) -> bool {
    const std::string_view left = items[static_cast<size_t>(lhs)];
    const std::string_view right = items[static_cast<size_t>(rhs)];

    if (distanceSort) {
      auto distanceMetersOf = [&](std::string_view item, float& outMeters) -> bool {
        std::string_view latView;
        std::string_view lonView;
        if (!objectMemberValue(item, "lat", latView) || !objectMemberValue(item, "lon", lonView)) {
          return false;
        }
        float lat = 0.0f;
        float lon = 0.0f;
        if (!numericOf(latView, lat) || !numericOf(lonView, lon)) {
          return false;
        }
        outMeters = distanceKm(originLat, originLon, lat, lon) * 1000.0f;
        return true;
      };

      float ld = 0.0f;
      float rd = 0.0f;
      const bool lOk = distanceMetersOf(left, ld);
      const bool rOk = distanceMetersOf(right, rd);
      if (lOk && rOk) {
        if (std::fabs(ld - rd) > 0.000001f) {
          return ld < rd;
        }
        return lhs < rhs;
      }
      if (lOk != rOk) {
        return lOk;
      }
      return lhs < rhs;
    }

    auto resolveKey = [&](std::string_view item, std::string_view& out) -> bool {
      if (keyPath.empty() || keyPath == "." || keyPath == "*") {
        out = item;
        return true;
      }
      return resolveJsonPath(item, keyPath, out);
    };
    std::string_view lKey;
    std::string_view rKey;
    const bool haveLeft = resolveKey(left, lKey);
    const bool haveRight = resolveKey(right, rKey);

    if (numericSort) {
      float lnum = 0.0f;
      float rnum = 0.0f;
      const bool lOk = haveLeft && numericOf(lKey, lnum);
      const bool rOk = haveRight && numericOf(rKey, rnum);
      if (lOk && rOk) {
        if (std::fabs(lnum - rnum) > 0.000001f) {
          return lnum < rnum;
        }
        return lhs < rhs;
      }
      if (lOk != rOk) {
        return lOk;
      }
      return lhs < rhs;
    }

    std::string ls = haveLeft ? valueViewToText(lKey) : std::string();
    std::string rs = haveRight ? valueViewToText(rKey) : std::string();
    std::transform(ls.begin(), ls.end(), ls.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    std::transform(rs.begin(), rs.end(), rs.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    const int cmp = ls.compare(rs);
    if (cmp != 0) {
      return cmp < 0;
    }
    return lhs < rhs;
  };

  std::stable_sort(indexOrder.begin(), indexOrder.end(), [&](int lhs, int rhs) {
    return descending ? cmpAsc(rhs, lhs) : cmpAsc(lhs, rhs);
  });

  s.transformJson.clear();
  s.transformJson.push_back('[');
  for (size_t i = 0; i < indexOrder.size(); ++i) {
    if (i > 0) {
      s.transformJson.push_back(',');
    }
    const std::string_view item = items[static_cast<size_t>(indexOrder[i])];
    s.transformJson.append(item.data(), item.size());
  }
  s.transformJson.push_back(']');

  if (tail.empty()) {
    outValue = std::string_view(s.transformJson);
    return true;
  }
  return resolveJsonPath(std::string_view(s.transformJson), tail, outValue);
}

bool resolveJsonPath(std::string_view rootText, const std::string& path, std::string_view& outValue) {
  std::string resolvedPath = bindRuntimeTemplate(path);
  std::string_view pathView = trimView(resolvedPath);
  if (pathView.empty()) {
    return false;
  }
  if (pathView.rfind("sort_num(", 0) == 0 || pathView.rfind("sort_alpha(", 0) == 0 ||
      pathView.rfind("distance_sort(", 0) == 0 || pathView.rfind("sort_distance(", 0) == 0) {
    return resolveSortPath(rootText, pathView, outValue);
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

bool readFloatValue(std::string_view obj, const char* key, const VarContext* vars, float& out) {
  std::string_view value;
  if (!objectMemberValue(obj, key, value)) {
    return false;
  }
  value = trimView(value);
  if (value.empty()) {
    return false;
  }
  if (value.front() == '"') {
    std::string expr;
    if (!viewToString(value, expr)) {
      return false;
    }
    expr = substituteTemplateVars(expr, vars);
    expr = bindRuntimeTemplate(expr);
    return evalNumericExpr(expr, vars, out);
  }
  double raw = 0.0;
  if (!viewToDouble(value, raw)) {
    return false;
  }
  out = static_cast<float>(raw);
  return true;
}

bool readIntValue(std::string_view obj, const char* key, const VarContext* vars, int& out) {
  float value = 0.0f;
  if (!readFloatValue(obj, key, vars, value)) {
    return false;
  }
  out = static_cast<int>(std::lround(value));
  return true;
}

std::string readStringValue(std::string_view obj, const char* key, const VarContext* vars,
                            const std::string& fallback = std::string()) {
  std::string value;
  if (!objectMemberString(obj, key, value)) {
    return fallback;
  }
  return substituteTemplateVars(value, vars);
}

NodeType parseNodeType(std::string_view nodeObj, bool& known) {
  std::string type;
  known = true;
  if (!objectMemberString(nodeObj, "type", type)) {
    return NodeType::kLabel;
  }
  std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (type == "label") return NodeType::kLabel;
  if (type == "value_box") return NodeType::kValueBox;
  if (type == "progress") return NodeType::kProgress;
  if (type == "sparkline") return NodeType::kSparkline;
  if (type == "icon") return NodeType::kIcon;
  if (type == "moon_phase") return NodeType::kMoonPhase;
  if (type == "arc" || type == "circle") return NodeType::kArc;
  if (type == "line" || type == "hand") return NodeType::kLine;
  known = false;
  return NodeType::kLabel;
}

HAlign parseHAlign(const std::string& value) {
  if (value == "center") {
    return HAlign::kCenter;
  }
  if (value == "right") {
    return HAlign::kRight;
  }
  return HAlign::kLeft;
}

VAlign parseVAlign(const std::string& value) {
  if (value == "center" || value == "middle") {
    return VAlign::kCenter;
  }
  if (value == "bottom") {
    return VAlign::kBottom;
  }
  return VAlign::kTop;
}

TextDatum parseDatum(const std::string& align, const std::string& valign) {
  std::string ha = align.empty() ? "left" : align;
  std::string va = valign.empty() ? "top" : valign;
  if (va == "top") {
    if (ha == "center") return TextDatum::kTC;
    if (ha == "right") return TextDatum::kTR;
    return TextDatum::kTL;
  }
  if (va == "middle" || va == "center") {
    if (ha == "center") return TextDatum::kMC;
    if (ha == "right") return TextDatum::kMR;
    return TextDatum::kML;
  }
  if (va == "bottom") {
    if (ha == "center") return TextDatum::kBC;
    if (ha == "right") return TextDatum::kBR;
    return TextDatum::kBL;
  }
  if (va == "baseline") {
    if (ha == "center") return TextDatum::kCBaseline;
    if (ha == "right") return TextDatum::kRBaseline;
    return TextDatum::kLBaseline;
  }
  return TextDatum::kTL;
}

void applyNode(std::string_view nodeObj, const VarContext* vars, std::vector<Node>& outNodes);

void applyNodes(std::string_view nodesArray, const VarContext* vars, std::vector<Node>& outNodes) {
  forEachArrayElement(nodesArray, [&](int /*idx*/, std::string_view nodeValue) {
    nodeValue = trimView(nodeValue);
    if (nodeValue.empty() || nodeValue.front() != '{') {
      return;
    }

    std::string type;
    if (!objectMemberString(nodeValue, "type", type)) {
      applyNode(nodeValue, vars, outNodes);
      return;
    }
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    if (type != "repeat") {
      applyNode(nodeValue, vars, outNodes);
      return;
    }

    int count = 0;
    (void)readIntValue(nodeValue, "count", vars, count);
    int times = 0;
    if (readIntValue(nodeValue, "times", vars, times)) {
      count = times;
    }
    count = std::clamp(count, 0, 512);
    if (count <= 0) {
      return;
    }

    float start = 0.0f;
    float step = 1.0f;
    (void)readFloatValue(nodeValue, "start", vars, start);
    (void)readFloatValue(nodeValue, "step", vars, step);
    std::string var = readStringValue(nodeValue, "var", vars, "i");
    if (var.empty()) {
      var = "i";
    }

    std::string_view childNodes;
    std::string_view singleNode;
    const bool hasNodes = objectMemberArray(nodeValue, "nodes", childNodes);
    const bool hasNode = objectMemberObject(nodeValue, "node", singleNode);
    for (int i = 0; i < count; ++i) {
      VarContext local;
      local.parent = vars;
      local.name = var;
      local.value = start + static_cast<float>(i) * step;
      if (hasNodes) {
        applyNodes(childNodes, &local, outNodes);
      } else if (hasNode) {
        applyNode(singleNode, &local, outNodes);
      }
    }
  });
}

void applyNode(std::string_view nodeObj, const VarContext* vars, std::vector<Node>& outNodes) {
  bool known = false;
  Node node;
  node.type = parseNodeType(nodeObj, known);
  if (!known) {
    return;
  }

  (void)readIntValue(nodeObj, "x", vars, node.x);
  (void)readIntValue(nodeObj, "y", vars, node.y);
  (void)readIntValue(nodeObj, "w", vars, node.w);
  (void)readIntValue(nodeObj, "h", vars, node.h);
  (void)readIntValue(nodeObj, "x2", vars, node.x2);
  (void)readIntValue(nodeObj, "y2", vars, node.y2);
  (void)readIntValue(nodeObj, "r", vars, node.radius);
  (void)readIntValue(nodeObj, "length", vars, node.length);
  (void)readIntValue(nodeObj, "thickness", vars, node.thickness);
  (void)readIntValue(nodeObj, "font", vars, node.font);
  (void)readIntValue(nodeObj, "line_height", vars, node.lineHeight);
  (void)readIntValue(nodeObj, "max_lines", vars, node.maxLines);
  (void)objectMemberBool(nodeObj, "wrap", node.wrap);

  float temp = 0.0f;
  if (readFloatValue(nodeObj, "min", vars, temp)) node.min = temp;
  if (readFloatValue(nodeObj, "max", vars, temp)) node.max = temp;
  if (readFloatValue(nodeObj, "start_deg", vars, temp)) node.startDeg = temp;
  if (readFloatValue(nodeObj, "end_deg", vars, temp)) node.endDeg = temp;

  node.text = readStringValue(nodeObj, "text", vars, "");
  node.key = readStringValue(nodeObj, "key", vars, "");
  node.path = readStringValue(nodeObj, "path", vars, "");
  if (node.path.empty()) {
    node.path = readStringValue(nodeObj, "icon", vars, "");
  }
  node.angleExpr = readStringValue(nodeObj, "angle_expr", vars, "");

  std::string overflow = readStringValue(nodeObj, "overflow", vars, "");
  std::transform(overflow.begin(), overflow.end(), overflow.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  node.overflow = (overflow == "ellipsis") ? OverflowMode::kEllipsis : OverflowMode::kClip;

  std::string align = readStringValue(nodeObj, "align", vars, "");
  std::transform(align.begin(), align.end(), align.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  node.align = parseHAlign(align);

  std::string valign = readStringValue(nodeObj, "valign", vars, "");
  std::transform(valign.begin(), valign.end(), valign.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  node.valign = parseVAlign(valign);
  node.datum = parseDatum(align, valign);

  std::string color = readStringValue(nodeObj, "color", vars, "#FFFFFF");
  if (!parseHexColor565(color, node.color565)) {
    node.color565 = 0xFFFF;
  }
  std::string bg = readStringValue(nodeObj, "bg", vars, "#000000");
  if (!parseHexColor565(bg, node.bg565)) {
    node.bg565 = 0x0000;
  }

  outNodes.push_back(std::move(node));
}

bool parseModalSpec(std::string_view modalObj, ModalSpec& out) {
  out = {};
  out.id = readStringValue(modalObj, "id", nullptr, "");
  out.title = readStringValue(modalObj, "title", nullptr, "");
  out.text = readStringValue(modalObj, "text", nullptr, "");
  (void)objectMemberInt(modalObj, "x", out.x);
  (void)objectMemberInt(modalObj, "y", out.y);
  (void)objectMemberInt(modalObj, "w", out.w);
  (void)objectMemberInt(modalObj, "h", out.h);
  (void)objectMemberInt(modalObj, "font", out.font);
  (void)objectMemberInt(modalObj, "line_height", out.lineHeight);
  (void)objectMemberInt(modalObj, "max_lines", out.maxLines);

  std::string color;
  color = readStringValue(modalObj, "bg", nullptr, "#101820");
  if (!parseHexColor565(color, out.bg565)) {
    out.bg565 = 0x10A2;
  }
  color = readStringValue(modalObj, "border", nullptr, "#4A90E2");
  if (!parseHexColor565(color, out.border565)) {
    out.border565 = 0x4C9C;
  }
  color = readStringValue(modalObj, "title_color", nullptr, "#FFFFFF");
  if (!parseHexColor565(color, out.titleColor565)) {
    out.titleColor565 = 0xFFFF;
  }
  color = readStringValue(modalObj, "text_color", nullptr, "#D8E6F5");
  if (!parseHexColor565(color, out.textColor565)) {
    out.textColor565 = 0xDF3E;
  }

  if (out.id.empty()) {
    return false;
  }
  if (out.w <= 0 || out.h <= 0) {
    return false;
  }
  return true;
}

TouchActionType parseTouchActionType(const std::string& action) {
  std::string lower = action;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (lower == "modal") {
    return TouchActionType::kModal;
  }
  if (lower == "http") {
    return TouchActionType::kHttp;
  }
  return TouchActionType::kNone;
}

bool parseTouchRegion(std::string_view regionObj, TouchRegion& out) {
  out = {};
  if (!objectMemberInt(regionObj, "x", out.x) || !objectMemberInt(regionObj, "y", out.y) ||
      !objectMemberInt(regionObj, "w", out.w) || !objectMemberInt(regionObj, "h", out.h)) {
    return false;
  }
  if (out.w <= 0 || out.h <= 0) {
    return false;
  }

  std::string_view onTouchObj;
  if (!objectMemberObject(regionObj, "on_touch", onTouchObj)) {
    return false;
  }
  const std::string action = readStringValue(onTouchObj, "action", nullptr, "");
  out.action = parseTouchActionType(action);
  if (out.action == TouchActionType::kModal) {
    out.modalId = readStringValue(onTouchObj, "modal_id", nullptr, "");
    int dismissMs = 0;
    if (objectMemberInt(onTouchObj, "dismiss_ms", dismissMs) && dismissMs > 0) {
      out.dismissMs = static_cast<uint32_t>(dismissMs);
    }
    if (out.modalId.empty()) {
      return false;
    }
  } else if (out.action == TouchActionType::kHttp) {
    out.httpUrl = readStringValue(onTouchObj, "url", nullptr, "");
    out.httpMethod = readStringValue(onTouchObj, "method", nullptr, "POST");
    out.httpBody = readStringValue(onTouchObj, "body", nullptr, "");
    out.httpContentType = readStringValue(onTouchObj, "content_type", nullptr, "application/json");

    std::string_view headersObj;
    if (objectMemberObject(onTouchObj, "headers", headersObj)) {
      forEachObjectMember(headersObj, [&](const std::string& key, std::string_view valueText) {
        std::string value;
        if (!viewToString(valueText, value)) {
          return;
        }
        const std::string trimmedKey = trimCopy(key);
        const std::string trimmedValue = trimCopy(value);
        if (!trimmedKey.empty() && !trimmedValue.empty()) {
          out.httpHeaders.push_back({trimmedKey, trimmedValue});
        }
      });
    }
    if (out.httpUrl.empty()) {
      return false;
    }
  }
  return out.action != TouchActionType::kNone;
}

struct TransformRow {
  std::map<std::string, std::string> fields;
};

bool parseStringArray(std::string_view arrayText, std::vector<std::string>& out) {
  out.clear();
  arrayText = trimView(arrayText);
  if (arrayText.size() < 2 || arrayText.front() != '[' || arrayText.back() != ']') {
    return false;
  }
  forEachArrayElement(arrayText, [&](int, std::string_view valueText) {
    std::string value;
    if (viewToString(valueText, value)) {
      out.push_back(value);
    }
  });
  return true;
}

std::string mapValueFromPath(std::string_view itemObj, const std::string& path) {
  std::string_view valueView;
  if (!resolveJsonPath(itemObj, path, valueView)) {
    return {};
  }
  return valueViewToText(valueView);
}

bool transformMap(std::string_view rootJson, std::string_view trObj,
                  std::map<std::string, std::vector<TransformRow>>& arrays) {
  std::string from = readStringValue(trObj, "from", nullptr, "");
  std::string to = readStringValue(trObj, "to", nullptr, "");
  if (from.empty() || to.empty()) {
    return false;
  }

  std::string_view sourceArrayView;
  if (!resolveJsonPath(rootJson, from, sourceArrayView)) {
    return false;
  }
  sourceArrayView = trimView(sourceArrayView);
  if (sourceArrayView.size() < 2 || sourceArrayView.front() != '[' || sourceArrayView.back() != ']') {
    return false;
  }

  std::string_view fieldsObj;
  if (!objectMemberObject(trObj, "fields", fieldsObj)) {
    return false;
  }

  std::vector<TransformRow> rows;
  forEachArrayElement(sourceArrayView, [&](int, std::string_view item) {
    item = trimView(item);
    if (item.empty() || item.front() != '{') {
      return;
    }
    TransformRow row;
    forEachObjectMember(fieldsObj, [&](const std::string& outField, std::string_view spec) {
      spec = trimView(spec);
      std::string outValue;
      if (!spec.empty() && spec.front() == '"') {
        std::string path;
        if (viewToString(spec, path)) {
          outValue = mapValueFromPath(item, path);
        }
      } else if (!spec.empty() && spec.front() == '{') {
        std::string_view coalesceArray;
        if (objectMemberArray(spec, "coalesce", coalesceArray)) {
          std::vector<std::string> paths;
          (void)parseStringArray(coalesceArray, paths);
          for (const std::string& path : paths) {
            outValue = mapValueFromPath(item, path);
            if (!trimCopy(outValue).empty()) {
              break;
            }
          }
        }
        if (trimCopy(outValue).empty()) {
          std::string fallback;
          if (objectMemberString(spec, "default", fallback)) {
            outValue = fallback;
          }
        }
      }
      row.fields[outField] = outValue;
    });
    rows.push_back(std::move(row));
  });

  arrays[to] = std::move(rows);
  return true;
}

bool rowNumeric(const TransformRow& row, const std::string& key, double& out) {
  auto it = row.fields.find(key);
  if (it == row.fields.end()) {
    return false;
  }
  return parseStrictDouble(it->second, out);
}

bool transformComputeDistance(std::string_view trObj, std::map<std::string, std::vector<TransformRow>>& arrays) {
  const std::string from = readStringValue(trObj, "from", nullptr, "");
  const std::string toField = readStringValue(trObj, "to_field", nullptr, "");
  if (from.empty() || toField.empty()) {
    return false;
  }
  auto it = arrays.find(from);
  if (it == arrays.end()) {
    return false;
  }

  const std::string preferNmPath = readStringValue(trObj, "prefer_nm_path", nullptr, "");
  const std::string latPath = readStringValue(trObj, "lat_path", nullptr, "lat");
  const std::string lonPath = readStringValue(trObj, "lon_path", nullptr, "lon");
  std::string latArg = readStringValue(trObj, "origin_lat", nullptr, "");
  std::string lonArg = readStringValue(trObj, "origin_lon", nullptr, "");
  latArg = bindRuntimeTemplate(latArg);
  lonArg = bindRuntimeTemplate(lonArg);
  double originLat = loadGeoLat();
  double originLon = loadGeoLon();
  (void)parseStrictDouble(latArg, originLat);
  (void)parseStrictDouble(lonArg, originLon);

  for (TransformRow& row : it->second) {
    double km = 0.0;
    bool have = false;
    if (!preferNmPath.empty()) {
      double nm = 0.0;
      if (rowNumeric(row, preferNmPath, nm)) {
        km = nm * 1.852;
        have = true;
      }
    }
    if (!have) {
      double lat = 0.0;
      double lon = 0.0;
      if (rowNumeric(row, latPath, lat) && rowNumeric(row, lonPath, lon)) {
        km = static_cast<double>(
            distanceKm(static_cast<float>(originLat), static_cast<float>(originLon), static_cast<float>(lat),
                       static_cast<float>(lon)));
        have = true;
      }
    }
    if (!have) {
      continue;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", km);
    row.fields[toField] = buf;
  }
  return true;
}

bool transformComputeOffset(std::string_view trObj, std::map<std::string, std::vector<TransformRow>>& arrays) {
  const std::string from = readStringValue(trObj, "from", nullptr, "");
  const std::string xField = readStringValue(trObj, "x_field", nullptr, "");
  const std::string yField = readStringValue(trObj, "y_field", nullptr, "");
  if (from.empty() || xField.empty() || yField.empty()) {
    return false;
  }
  auto it = arrays.find(from);
  if (it == arrays.end()) {
    return false;
  }

  const std::string latPath = readStringValue(trObj, "lat_path", nullptr, "lat");
  const std::string lonPath = readStringValue(trObj, "lon_path", nullptr, "lon");
  std::string latArg = readStringValue(trObj, "origin_lat", nullptr, "");
  std::string lonArg = readStringValue(trObj, "origin_lon", nullptr, "");
  latArg = bindRuntimeTemplate(latArg);
  lonArg = bindRuntimeTemplate(lonArg);
  double originLat = loadGeoLat();
  double originLon = loadGeoLon();
  (void)parseStrictDouble(latArg, originLat);
  (void)parseStrictDouble(lonArg, originLon);

  for (TransformRow& row : it->second) {
    double lat = 0.0;
    double lon = 0.0;
    if (!rowNumeric(row, latPath, lat) || !rowNumeric(row, lonPath, lon)) {
      continue;
    }
    const double avgLatRad = ((originLat + lat) * 0.5) * (M_PI / 180.0);
    const double dxKm = (lon - originLon) * 111.320 * std::cos(avgLatRad);
    const double dyKm = (lat - originLat) * 110.574;
    char xb[32];
    char yb[32];
    std::snprintf(xb, sizeof(xb), "%.3f", dxKm);
    std::snprintf(yb, sizeof(yb), "%.3f", dyKm);
    row.fields[xField] = xb;
    row.fields[yField] = yb;
  }
  return true;
}

bool transformFilterLte(std::string_view trObj, std::map<std::string, std::vector<TransformRow>>& arrays) {
  const std::string from = readStringValue(trObj, "from", nullptr, "");
  const std::string by = readStringValue(trObj, "by", nullptr, "");
  if (from.empty() || by.empty()) {
    return false;
  }
  auto it = arrays.find(from);
  if (it == arrays.end()) {
    return false;
  }

  std::string maxText = readStringValue(trObj, "max", nullptr, "");
  maxText = bindRuntimeTemplate(maxText);
  if (maxText.empty()) {
    return false;
  }
  double maxValue = 0.0;
  if (!parseStrictDouble(maxText, maxValue)) {
    return false;
  }

  std::string unit = readStringValue(trObj, "unit", nullptr, "km");
  std::transform(unit.begin(), unit.end(), unit.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (unit == "mi" || unit == "mile" || unit == "miles") {
    maxValue *= 1.609344;
  } else if (unit == "nm") {
    maxValue *= 1.852;
  }

  std::vector<TransformRow> filtered;
  filtered.reserve(it->second.size());
  for (const TransformRow& row : it->second) {
    double value = 0.0;
    if (!rowNumeric(row, by, value)) {
      continue;
    }
    if (value <= maxValue) {
      filtered.push_back(row);
    }
  }
  it->second = std::move(filtered);
  return true;
}

bool transformSort(std::string_view trObj, std::map<std::string, std::vector<TransformRow>>& arrays) {
  const std::string from = readStringValue(trObj, "from", nullptr, "");
  const std::string by = readStringValue(trObj, "by", nullptr, "");
  if (from.empty() || by.empty()) {
    return false;
  }
  auto it = arrays.find(from);
  if (it == arrays.end()) {
    return false;
  }

  bool numeric = false;
  (void)objectMemberBool(trObj, "numeric", numeric);
  std::string order = readStringValue(trObj, "order", nullptr, "asc");
  std::transform(order.begin(), order.end(), order.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  const bool desc = (order == "desc" || order == "reverse" || order == "rev");

  std::stable_sort(it->second.begin(), it->second.end(), [&](const TransformRow& a, const TransformRow& b) {
    auto aIt = a.fields.find(by);
    auto bIt = b.fields.find(by);
    std::string av = (aIt != a.fields.end()) ? aIt->second : "";
    std::string bv = (bIt != b.fields.end()) ? bIt->second : "";
    bool less = false;
    if (numeric) {
      double an = 0.0;
      double bn = 0.0;
      const bool aOk = parseStrictDouble(av, an);
      const bool bOk = parseStrictDouble(bv, bn);
      if (aOk && bOk) {
        less = an < bn;
      } else {
        less = aOk && !bOk;
      }
    } else {
      std::transform(av.begin(), av.end(), av.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      std::transform(bv.begin(), bv.end(), bv.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      less = av < bv;
    }
    return desc ? !less : less;
  });
  return true;
}

bool transformTake(std::string_view trObj, std::map<std::string, std::vector<TransformRow>>& arrays) {
  const std::string from = readStringValue(trObj, "from", nullptr, "");
  int count = 0;
  if (from.empty() || !objectMemberInt(trObj, "count", count) || count < 0) {
    return false;
  }
  auto it = arrays.find(from);
  if (it == arrays.end()) {
    return false;
  }
  if (static_cast<size_t>(count) < it->second.size()) {
    it->second.resize(static_cast<size_t>(count));
  }
  return true;
}

std::string formatDistanceFromKm(double km) {
  const double dist = RuntimeSettings::useMiles ? (km * 0.621371) : km;
  const char* unit = RuntimeSettings::useMiles ? "mi" : "km";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f%s", dist, unit);
  return buf;
}

std::string synthesizeLine(const TransformRow& row) {
  const auto pick = [&](const char* key, const char* fallback = "") -> std::string {
    auto it = row.fields.find(key);
    return (it != row.fields.end() && !it->second.empty()) ? it->second : std::string(fallback);
  };
  std::string flight = pick("flight", "?");
  std::string distance = pick("distance_text", "");
  if (distance.empty()) {
    double km = 0.0;
    if (rowNumeric(row, "km", km)) {
      distance = formatDistanceFromKm(km);
    }
  }
  std::string alt = pick("alt_text", "?");
  if (alt == "ground" || alt == "GROUND") {
    alt = "GND";
  } else {
    double altitude = 0.0;
    if (parseStrictDouble(alt, altitude)) {
      alt = std::to_string(static_cast<int>(std::lround(altitude))) + "ft";
    }
  }
  std::string type = pick("type", "?");
  std::string dest = pick("dest", "?");
  return flight + " " + distance + " " + alt + " " + type + "->" + dest;
}

bool transformIndexRows(std::string_view trObj, std::map<std::string, std::vector<TransformRow>>& arrays,
                        std::map<std::string, std::string>& outFlat) {
  const std::string from = readStringValue(trObj, "from", nullptr, "");
  if (from.empty()) {
    return false;
  }
  auto it = arrays.find(from);
  if (it == arrays.end()) {
    return false;
  }

  int count = 5;
  (void)objectMemberInt(trObj, "count", count);
  if (count < 0) {
    count = 0;
  }
  bool fillEmpty = true;
  (void)objectMemberBool(trObj, "fill_empty", fillEmpty);
  const std::string countKey = readStringValue(trObj, "count_key", nullptr, "count");

  std::vector<std::string> fields;
  std::string_view fieldsArray;
  if (objectMemberArray(trObj, "fields", fieldsArray)) {
    (void)parseStringArray(fieldsArray, fields);
  }
  if (fields.empty()) {
    fields = {"line"};
  }

  std::map<std::string, std::string> prefixMap;
  std::string_view prefixObj;
  if (objectMemberObject(trObj, "prefix_map", prefixObj)) {
    forEachObjectMember(prefixObj, [&](const std::string& key, std::string_view valueText) {
      std::string value;
      if (viewToString(valueText, value) && !value.empty()) {
        prefixMap[key] = value;
      }
    });
  }

  outFlat[countKey] = std::to_string(static_cast<int>(std::min<size_t>(it->second.size(), static_cast<size_t>(count))));
  for (int i = 0; i < count; ++i) {
    const bool have = static_cast<size_t>(i) < it->second.size();
    const TransformRow* row = have ? &it->second[static_cast<size_t>(i)] : nullptr;
    for (const std::string& field : fields) {
      const std::string prefix = prefixMap.count(field) != 0 ? prefixMap[field] : field;
      const std::string key = prefix + std::to_string(i + 1);
      std::string value;
      if (row != nullptr) {
        auto fit = row->fields.find(field);
        if (fit != row->fields.end()) {
          value = fit->second;
        } else if (field == "line") {
          value = synthesizeLine(*row);
        }
      }
      if (row == nullptr && !fillEmpty) {
        continue;
      }
      outFlat[key] = value;
    }
  }
  return true;
}

void applyTransforms(std::string_view rootJson) {
  std::map<std::string, std::vector<TransformRow>> arrays;
  std::map<std::string, std::string> outFlat;

  for (const std::string& raw : s.transforms) {
    std::string_view trObj = trimView(raw);
    if (trObj.empty() || trObj.front() != '{') {
      continue;
    }
    std::string op = readStringValue(trObj, "op", nullptr, "");
    std::transform(op.begin(), op.end(), op.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    bool ok = false;
    if (op == "map") {
      ok = transformMap(rootJson, trObj, arrays);
    } else if (op == "compute_distance") {
      ok = transformComputeDistance(trObj, arrays);
    } else if (op == "compute_offset") {
      ok = transformComputeOffset(trObj, arrays);
    } else if (op == "filter_lte") {
      ok = transformFilterLte(trObj, arrays);
    } else if (op == "sort") {
      ok = transformSort(trObj, arrays);
    } else if (op == "take") {
      ok = transformTake(trObj, arrays);
    } else if (op == "index_rows") {
      ok = transformIndexRows(trObj, arrays, outFlat);
    }
    if (!ok && s.debug) {
      ESP_LOGW(kTag, "transform op failed: %s", op.c_str());
    }
  }

  for (const auto& prev : s.transformValues) {
    if (outFlat.find(prev.first) == outFlat.end()) {
      (void)setValue(prev.first, "");
      s.numericValues.erase(prev.first);
    }
  }
  for (const auto& kv : outFlat) {
    (void)setValue(kv.first, kv.second);
    double numeric = 0.0;
    if (parseStrictDouble(kv.second, numeric)) {
      s.numericValues[kv.first] = numeric;
    } else {
      s.numericValues.erase(kv.first);
    }
  }
  s.transformValues = std::move(outFlat);
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
  } else if (source == "ha_ws") {
    s.source = DataSource::kHaWs;
  } else if (source == "local_time") {
    s.source = DataSource::kLocalTime;
  } else {
    s.source = DataSource::kUnknown;
  }

  s.urlTemplate.clear();
  s.wsEntityTemplate.clear();
  s.headers.clear();
  s.transforms.clear();
  (void)objectMemberString(dataObj, "url", s.urlTemplate);
  (void)objectMemberString(dataObj, "entity_id", s.wsEntityTemplate);
  std::string_view headersObj;
  if (objectMemberObject(dataObj, "headers", headersObj)) {
    forEachObjectMember(headersObj, [](const std::string& key, std::string_view valueText) {
      std::string value;
      if (!viewToString(valueText, value)) {
        return;
      }
      const std::string trimmedKey = trimCopy(key);
      const std::string trimmedValue = trimCopy(value);
      if (trimmedKey.empty() || trimmedValue.empty()) {
        return;
      }
      s.headers.push_back({trimmedKey, trimmedValue});
    });
  }
  std::string_view transformsArray;
  if (objectMemberArray(dataObj, "transforms", transformsArray)) {
    forEachArrayElement(transformsArray, [&](int, std::string_view valueText) {
      valueText = trimView(valueText);
      if (valueText.empty() || valueText.front() != '{') {
        return;
      }
      s.transforms.emplace_back(valueText.data(), valueText.size());
    });
  }

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
  s.numericValues.clear();
  s.transformValues.clear();
  s.nodes.clear();
  s.modals.clear();
  s.touchRegions.clear();
  s.activeModalId.clear();
  s.httpMaxBytes = kHttpResponseMaxBytesDefault;
  s.retainSourceJson = false;

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
    std::string_view labelsArray;
    if (objectMemberArray(uiObj, "labels", labelsArray)) {
      forEachArrayElement(labelsArray, [&](int, std::string_view value) {
        value = trimView(value);
        if (value.empty() || value.front() != '{') {
          return;
        }
        Node n;
        n.type = NodeType::kLabel;
        (void)objectMemberInt(value, "x", n.x);
        (void)objectMemberInt(value, "y", n.y);
        (void)objectMemberInt(value, "font", n.font);
        (void)objectMemberString(value, "text", n.text);
        std::string color;
        if (objectMemberString(value, "color", color) && parseHexColor565(color, n.color565)) {
          // Parsed custom color.
        }
        if (!n.text.empty()) {
          s.nodes.push_back(std::move(n));
        }
      });
    }
    std::string_view nodesArray;
    if (objectMemberArray(uiObj, "nodes", nodesArray)) {
      applyNodes(nodesArray, nullptr, s.nodes);
    }

    std::string_view modalsArray;
    if (objectMemberArray(uiObj, "modals", modalsArray)) {
      forEachArrayElement(modalsArray, [&](int, std::string_view modalValue) {
        modalValue = trimView(modalValue);
        if (modalValue.empty() || modalValue.front() != '{') {
          return;
        }
        ModalSpec modal;
        if (parseModalSpec(modalValue, modal)) {
          s.modals.push_back(std::move(modal));
        }
      });
    }

    std::string_view touchRegionsArray;
    if (objectMemberArray(uiObj, "touch_regions", touchRegionsArray)) {
      forEachArrayElement(touchRegionsArray, [&](int, std::string_view regionValue) {
        regionValue = trimView(regionValue);
        if (regionValue.empty() || regionValue.front() != '{') {
          return;
        }
        TouchRegion region;
        if (parseTouchRegion(regionValue, region)) {
          s.touchRegions.push_back(std::move(region));
        }
      });
    }
  }

  if (s.source == DataSource::kHttp && s.urlTemplate.empty()) {
    ESP_LOGE(kTag, "dsl missing data.url for http source");
    return false;
  }
  if (s.source == DataSource::kHaWs && s.wsEntityTemplate.empty()) {
    s.wsEntityTemplate = "{{setting.entity_id}}";
  }
  if (auto it = s.settingValues.find("http_max_bytes"); it != s.settingValues.end()) {
    double parsed = 0.0;
    if (parseStrictDouble(it->second, parsed) && std::isfinite(parsed)) {
      const uint32_t v = static_cast<uint32_t>(std::lround(parsed));
      s.httpMaxBytes = std::clamp<uint32_t>(v, kHttpResponseMaxBytesMin, kHttpResponseMaxBytesMax);
    }
  }

  for (const Node& n : s.nodes) {
    if (!n.path.empty()) {
      s.retainSourceJson = true;
      break;
    }
  }

  if (s.nodes.empty()) {
    Node fallback;
    fallback.type = NodeType::kLabel;
    fallback.x = 8;
    fallback.y = 26;
    fallback.text = "DSL loaded";
    s.nodes.push_back(std::move(fallback));
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
  if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key != nullptr && evt->header_value != nullptr) {
    const char* key = static_cast<const char*>(evt->header_key);
    std::string keyLower = key;
    std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    if (keyLower == "content-length") {
      const char* val = static_cast<const char*>(evt->header_value);
      char* end = nullptr;
      const unsigned long parsed = std::strtoul(val, &end, 10);
      if (end != val && parsed > 0) {
        const size_t contentLen = static_cast<size_t>(parsed);
        if (contentLen > cap->maxBytes) {
          cap->overflow = true;
        } else {
          cap->body.reserve(contentLen);
        }
      }
    }
  }
  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data != nullptr && evt->data_len > 0) {
    if (cap->overflow) {
      return ESP_OK;
    }
    if (cap->body.size() + static_cast<size_t>(evt->data_len) > cap->maxBytes) {
      cap->overflow = true;
      return ESP_OK;
    }
    cap->body.append(static_cast<const char*>(evt->data), static_cast<size_t>(evt->data_len));
  }
  return ESP_OK;
}

std::string hostFromUrl(const std::string& url) {
  const size_t scheme = url.find("://");
  size_t start = (scheme == std::string::npos) ? 0 : (scheme + 3);
  const size_t end = url.find_first_of(":/?", start);
  if (start >= url.size()) {
    return {};
  }
  return end == std::string::npos ? url.substr(start) : url.substr(start, end - start);
}

bool isProxyUrl(const std::string& url, const std::string& host) {
  if (host.find("gorkos.net") != std::string::npos || host.find("image_proxy") != std::string::npos) {
    return true;
  }
  return url.find("/cmh?") != std::string::npos || url.find("/rss") != std::string::npos;
}

void parseSettingsJsonIntoMap(const char* settingsJson, std::map<std::string, std::string>& outMap) {
  if (settingsJson == nullptr || *settingsJson == '\0') {
    return;
  }
  const std::string_view root = trimView(settingsJson);
  if (root.empty() || root.front() != '{') {
    return;
  }
  forEachObjectMember(root, [&](const std::string& key, std::string_view valueText) {
    const std::string trimmedKey = trimCopy(key);
    if (trimmedKey.empty()) {
      return;
    }
    std::string value = valueViewToText(valueText);
    value = trimCopy(value);
    outMap[trimmedKey] = value;
  });
}

void loadWidgetSettings(const char* settingsJson, const char* sharedSettingsJson) {
  s.settingValues.clear();
  // Layout-level shared settings first, widget-specific settings override.
  parseSettingsJsonIntoMap(sharedSettingsJson, s.settingValues);
  parseSettingsJsonIntoMap(settingsJson, s.settingValues);
}

std::string readSetting(const std::string& key, const std::string& fallback) {
  auto it = s.settingValues.find(key);
  if (it == s.settingValues.end()) {
    return fallback;
  }
  return it->second;
}

bool takeHaWsLock(uint32_t timeoutMs) {
  if (sHaWs.lock == nullptr) {
    sHaWs.lock = xSemaphoreCreateMutex();
    if (sHaWs.lock == nullptr) {
      return false;
    }
  }
  return xSemaphoreTake(sHaWs.lock, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void giveHaWsLock() {
  if (sHaWs.lock != nullptr) {
    xSemaphoreGive(sHaWs.lock);
  }
}

std::string jsonEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

uint32_t nextHaWsReqIdLocked() {
  if (sHaWs.nextReqId == 0) {
    sHaWs.nextReqId = 1;
  }
  return sHaWs.nextReqId++;
}

void haWsSendAuthLocked() {
  if (sHaWs.client == nullptr || sHaWs.token.empty()) {
    return;
  }
  const std::string msg =
      std::string("{\"type\":\"auth\",\"access_token\":\"") + jsonEscape(sHaWs.token) + "\"}";
  (void)esp_websocket_client_send_text(sHaWs.client, msg.c_str(), static_cast<int>(msg.size()),
                                       pdMS_TO_TICKS(1000));
}

bool normalizeHaWsUrl(const std::string& baseUrl, const std::string& wsPath, std::string& out) {
  out.clear();
  const std::string trimmedBase = trimCopy(baseUrl);
  if (trimmedBase.empty()) {
    return false;
  }
  if (trimmedBase.rfind("ws://", 0) == 0 || trimmedBase.rfind("wss://", 0) == 0) {
    out = trimmedBase;
  } else if (trimmedBase.rfind("http://", 0) == 0) {
    out = "ws://" + trimmedBase.substr(7);
  } else if (trimmedBase.rfind("https://", 0) == 0) {
    out = "wss://" + trimmedBase.substr(8);
  } else {
    out = "wss://" + trimmedBase;
  }
  if (out.find("/api/websocket") == std::string::npos) {
    std::string path = trimCopy(wsPath);
    if (path.empty()) {
      path = "/api/websocket";
    }
    if (path.front() != '/') {
      path.insert(path.begin(), '/');
    }
    if (!out.empty() && out.back() == '/') {
      out.pop_back();
    }
    out += path;
  }
  return true;
}

void ingestHaStateObjectLocked(std::string_view stateObj) {
  std::string entityId;
  if (!objectMemberString(stateObj, "entity_id", entityId) || entityId.empty()) {
    return;
  }
  sHaWs.entityStateJson[entityId] = std::string(stateObj);
}

bool parseHaServiceFromUrl(const std::string& url, std::string& domainOut, std::string& serviceOut) {
  domainOut.clear();
  serviceOut.clear();
  const std::string marker = "/api/services/";
  const size_t pos = url.find(marker);
  if (pos == std::string::npos) {
    return false;
  }
  std::string tail = url.substr(pos + marker.size());
  const size_t queryPos = tail.find_first_of("?#");
  if (queryPos != std::string::npos) {
    tail = tail.substr(0, queryPos);
  }
  while (!tail.empty() && tail.front() == '/') {
    tail.erase(tail.begin());
  }
  const size_t slash = tail.find('/');
  if (slash == std::string::npos) {
    return false;
  }
  domainOut = trimCopy(tail.substr(0, slash));
  serviceOut = trimCopy(tail.substr(slash + 1));
  const size_t trailingSlash = serviceOut.find('/');
  if (trailingSlash != std::string::npos) {
    serviceOut = serviceOut.substr(0, trailingSlash);
  }
  return !domainOut.empty() && !serviceOut.empty();
}

bool haWsCallService(const std::string& domain, const std::string& service, const std::string& serviceDataJson,
                     std::string& reasonOut) {
  reasonOut.clear();
  if (domain.empty() || service.empty()) {
    reasonOut = "ha_ws service empty";
    return false;
  }

  std::string body = trimCopy(serviceDataJson);
  if (body.empty()) {
    body = "{}";
  }
  if (body.size() < 2 || body.front() != '{' || body.back() != '}') {
    reasonOut = "tap_body not object";
    return false;
  }

  if (!takeHaWsLock(200)) {
    reasonOut = "ws lock timeout";
    return false;
  }
  if (sHaWs.client == nullptr || !sHaWs.started || !sHaWs.authOk || !sHaWs.ready) {
    giveHaWsLock();
    reasonOut = "ws not ready";
    return false;
  }

  const uint32_t reqId = nextHaWsReqIdLocked();
  char prefix[192];
  std::snprintf(prefix, sizeof(prefix), "{\"id\":%u,\"type\":\"call_service\",\"domain\":\"%s\",\"service\":\"%s\","
                                        "\"service_data\":",
                static_cast<unsigned>(reqId), jsonEscape(domain).c_str(), jsonEscape(service).c_str());
  std::string msg(prefix);
  msg += body;
  msg.push_back('}');
  const int sent = esp_websocket_client_send_text(sHaWs.client, msg.c_str(), static_cast<int>(msg.size()),
                                                  pdMS_TO_TICKS(1000));
  giveHaWsLock();
  if (sent < 0) {
    reasonOut = "ws send failed";
    return false;
  }
  return true;
}

void processHaWsMessageLocked(std::string_view msg) {
  std::string type;
  if (!objectMemberString(msg, "type", type) || type.empty()) {
    return;
  }

  if (type == "auth_required") {
    sHaWs.authOk = false;
    sHaWs.ready = false;
    haWsSendAuthLocked();
    return;
  }
  if (type == "auth_ok") {
    sHaWs.authOk = true;
    sHaWs.ready = true;
    ESP_LOGI(kTag, "ha_ws auth_ok ready");
    int triggered = 0;
    for (State& inst : sInstances) {
      if (!inst.active || inst.source != DataSource::kHaWs) {
        continue;
      }
      inst.lastFetchMs = 0;
      inst.backoffUntilMs = 0;
      ++triggered;
    }
    ESP_LOGI(kTag, "ha_ws bootstrap trigger widgets=%d", triggered);
    return;
  }
  if (type == "auth_invalid") {
    sHaWs.authOk = false;
    sHaWs.ready = false;
    return;
  }
  if (type == "result") {
    int id = 0;
    bool success = false;
    (void)objectMemberInt(msg, "id", id);
    (void)objectMemberBool(msg, "success", success);
    auto triggerReqIt = sHaWs.triggerReqToEntityId.find(static_cast<uint32_t>(id));
    if (triggerReqIt != sHaWs.triggerReqToEntityId.end()) {
      const std::string entityId = triggerReqIt->second;
      sHaWs.triggerReqToEntityId.erase(triggerReqIt);
      sHaWs.entityIdToTriggerReq.erase(entityId);
      if (!success) {
        ESP_LOGW(kTag, "ha_ws trigger subscribe fail entity=%s", entityId.c_str());
        return;
      }
      sHaWs.triggerSubIdToEntityId[static_cast<uint32_t>(id)] = entityId;
      sHaWs.entityIdToTriggerSubId[entityId] = static_cast<uint32_t>(id);
      ESP_LOGI(kTag, "ha_ws trigger subscribed entity=%s", entityId.c_str());
      return;
    }
    auto renderIt = sHaWs.renderReqToEntityId.find(static_cast<uint32_t>(id));
    if (renderIt != sHaWs.renderReqToEntityId.end()) {
      const std::string entityId = renderIt->second;
      if (!success) {
        sHaWs.renderReqToEntityId.erase(renderIt);
        sHaWs.entityIdToRenderReq.erase(entityId);
        ESP_LOGW(kTag, "ha_ws bootstrap fail entity=%s", entityId.c_str());
        return;
      }
      std::string_view resultValue;
      if (objectMemberValue(msg, "result", resultValue)) {
        resultValue = trimView(resultValue);
        if (resultValue == "null") {
          // render_template often ACKs with null and sends actual value via a follow-up event.
          ESP_LOGI(kTag, "ha_ws bootstrap ack entity=%s awaiting_event", entityId.c_str());
          return;
        }
        if (resultValue.size() >= 2 && resultValue.front() == '{' && resultValue.back() == '}') {
          ingestHaStateObjectLocked(resultValue);
          sHaWs.renderReqToEntityId.erase(renderIt);
          sHaWs.entityIdToRenderReq.erase(entityId);
          ESP_LOGI(kTag, "ha_ws bootstrap ok entity=%s", entityId.c_str());
          return;
        }
        std::string rendered;
        if (viewToString(resultValue, rendered) && !rendered.empty()) {
          std::string_view renderedView = trimView(rendered);
          if (renderedView.size() >= 2 && renderedView.front() == '{' && renderedView.back() == '}') {
            ingestHaStateObjectLocked(renderedView);
            sHaWs.renderReqToEntityId.erase(renderIt);
            sHaWs.entityIdToRenderReq.erase(entityId);
            ESP_LOGI(kTag, "ha_ws bootstrap ok entity=%s", entityId.c_str());
            return;
          }
        }
        sHaWs.renderReqToEntityId.erase(renderIt);
        sHaWs.entityIdToRenderReq.erase(entityId);
        const size_t previewLen = std::min<size_t>(resultValue.size(), 120);
        ESP_LOGW(kTag, "ha_ws bootstrap empty entity=%s result=%.*s", entityId.c_str(),
                 static_cast<int>(previewLen), resultValue.data());
        return;
      }
      sHaWs.renderReqToEntityId.erase(renderIt);
      sHaWs.entityIdToRenderReq.erase(entityId);
      ESP_LOGW(kTag, "ha_ws bootstrap empty entity=%s missing_result", entityId.c_str());
      return;
    }
    if (!success) {
      return;
    }
    return;
  }
  if (type == "event") {
    int id = 0;
    (void)objectMemberInt(msg, "id", id);
    auto renderIt = sHaWs.renderReqToEntityId.find(static_cast<uint32_t>(id));
    if (renderIt != sHaWs.renderReqToEntityId.end()) {
      const std::string entityId = renderIt->second;
      std::string_view eventObj;
      if (!objectMemberObject(msg, "event", eventObj)) {
        sHaWs.renderReqToEntityId.erase(renderIt);
        sHaWs.entityIdToRenderReq.erase(entityId);
        ESP_LOGW(kTag, "ha_ws bootstrap empty entity=%s missing_event", entityId.c_str());
        return;
      }
      std::string rendered;
      if (!objectMemberString(eventObj, "result", rendered) || rendered.empty()) {
        sHaWs.renderReqToEntityId.erase(renderIt);
        sHaWs.entityIdToRenderReq.erase(entityId);
        ESP_LOGW(kTag, "ha_ws bootstrap empty entity=%s event_no_result", entityId.c_str());
        return;
      }
      std::string_view renderedView = trimView(rendered);
      if (renderedView.size() >= 2 && renderedView.front() == '{' && renderedView.back() == '}') {
        ingestHaStateObjectLocked(renderedView);
        sHaWs.renderReqToEntityId.erase(renderIt);
        sHaWs.entityIdToRenderReq.erase(entityId);
        ESP_LOGI(kTag, "ha_ws bootstrap ok entity=%s", entityId.c_str());
        return;
      }
      sHaWs.renderReqToEntityId.erase(renderIt);
      sHaWs.entityIdToRenderReq.erase(entityId);
      ESP_LOGW(kTag, "ha_ws bootstrap empty entity=%s event_result=%s", entityId.c_str(), rendered.c_str());
      return;
    }

    auto triggerSubIt = sHaWs.triggerSubIdToEntityId.find(static_cast<uint32_t>(id));
    if (triggerSubIt != sHaWs.triggerSubIdToEntityId.end()) {
      std::string_view eventObj;
      if (!objectMemberObject(msg, "event", eventObj)) {
        return;
      }
      std::string_view variablesObj;
      if (!objectMemberObject(eventObj, "variables", variablesObj)) {
        return;
      }
      std::string_view triggerObj;
      if (!objectMemberObject(variablesObj, "trigger", triggerObj)) {
        return;
      }
      std::string_view toStateObj;
      if (!objectMemberObject(triggerObj, "to_state", toStateObj)) {
        return;
      }
      ingestHaStateObjectLocked(toStateObj);
      return;
    }
    return;
  }
}

void haWsEventHandler(void* /*handler_args*/, esp_event_base_t base, int32_t event_id, void* event_data) {
  if (base != WEBSOCKET_EVENTS || event_data == nullptr) {
    return;
  }
  auto* data = static_cast<esp_websocket_event_data_t*>(event_data);
  if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
    if (takeHaWsLock(50)) {
      const uint32_t nowMs = platform::millisMs();
      sHaWs.started = false;
      sHaWs.authOk = false;
      sHaWs.ready = false;
      sHaWs.rxFrame.clear();
      sHaWs.renderReqToEntityId.clear();
      sHaWs.entityIdToRenderReq.clear();
      sHaWs.triggerReqToEntityId.clear();
      sHaWs.entityIdToTriggerReq.clear();
      sHaWs.triggerSubIdToEntityId.clear();
      sHaWs.entityIdToTriggerSubId.clear();
      if (sHaWs.failureStreak < 32) {
        ++sHaWs.failureStreak;
      }
      const uint32_t backoff = std::min<uint32_t>(60000U, 1000U << std::min<uint8_t>(sHaWs.failureStreak, 6));
      sHaWs.reconnectDueMs = nowMs + backoff;
      const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      const size_t freeNow = heap_caps_get_free_size(MALLOC_CAP_8BIT);
      ESP_LOGW(kTag, "ha_ws disconnected backoff_ms=%u heap_largest=%u heap_free=%u",
               static_cast<unsigned>(backoff), static_cast<unsigned>(largest), static_cast<unsigned>(freeNow));
      giveHaWsLock();
    }
    return;
  }
  if (event_id != WEBSOCKET_EVENT_DATA || data->data_ptr == nullptr || data->data_len <= 0 ||
      data->op_code != 0x1) {
    return;
  }
  const size_t totalLen = data->payload_len > 0 ? static_cast<size_t>(data->payload_len)
                                                : static_cast<size_t>(data->data_len);
  if (totalLen >= kHaWsDiagLargeFrameBytes && data->payload_offset == 0) {
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const size_t freeNow = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGW(kTag, "ha_ws large frame payload_len=%u heap_largest=%u heap_free=%u",
             static_cast<unsigned>(totalLen), static_cast<unsigned>(largest), static_cast<unsigned>(freeNow));
  }
  if (totalLen > kHaWsMaxFrameBytes) {
    ESP_LOGW(kTag, "ha_ws drop frame payload_len=%u cap=%u", static_cast<unsigned>(totalLen),
             static_cast<unsigned>(kHaWsMaxFrameBytes));
    return;
  }
  if (!takeHaWsLock(100)) {
    return;
  }
  if (data->payload_offset == 0) {
    sHaWs.rxFrame.clear();
  }
  if (sHaWs.rxFrame.size() + static_cast<size_t>(data->data_len) > kHaWsMaxFrameBytes) {
    ESP_LOGW(kTag, "ha_ws drop frame growth=%u cap=%u",
             static_cast<unsigned>(sHaWs.rxFrame.size() + static_cast<size_t>(data->data_len)),
             static_cast<unsigned>(kHaWsMaxFrameBytes));
    sHaWs.rxFrame.clear();
    giveHaWsLock();
    return;
  }
  sHaWs.rxFrame.append(static_cast<const char*>(data->data_ptr), static_cast<size_t>(data->data_len));
  const int total = data->payload_len;
  if (total > 0 && (data->payload_offset + data->data_len) >= total) {
    processHaWsMessageLocked(sHaWs.rxFrame);
    sHaWs.rxFrame.clear();
  }
  giveHaWsLock();
}

bool ensureHaWsConnected(const std::string& wsUrl, const std::string& token, const std::string& widgetId,
                         std::string& reasonOut) {
  reasonOut.clear();
  if (wsUrl.empty() || token.empty()) {
    reasonOut = "ws url/token empty";
    return false;
  }

  if (!takeHaWsLock(300)) {
    reasonOut = "ws lock timeout";
    return false;
  }

  const uint32_t nowMs = platform::millisMs();
  const bool configChanged = (sHaWs.wsUrl != wsUrl || sHaWs.token != token);
  if (configChanged) {
    if (sHaWs.client != nullptr) {
      esp_websocket_client_stop(sHaWs.client);
      esp_websocket_client_destroy(sHaWs.client);
      sHaWs.client = nullptr;
    }
    sHaWs.wsUrl = wsUrl;
    sHaWs.token = token;
    sHaWs.authOk = false;
    sHaWs.ready = false;
    sHaWs.started = false;
    sHaWs.entityStateJson.clear();
    sHaWs.renderReqToEntityId.clear();
    sHaWs.entityIdToRenderReq.clear();
    sHaWs.triggerReqToEntityId.clear();
    sHaWs.entityIdToTriggerReq.clear();
    sHaWs.triggerSubIdToEntityId.clear();
    sHaWs.entityIdToTriggerSubId.clear();
    sHaWs.failureStreak = 0;
    sHaWs.reconnectDueMs = 0;
    sHaWs.nextReqId = 1;
  }

  if (sHaWs.client == nullptr) {
    esp_websocket_client_config_t cfg = {};
    cfg.uri = sHaWs.wsUrl.c_str();
    cfg.task_prio = 4;
    cfg.task_stack = 6144;
    cfg.network_timeout_ms = static_cast<int>(kHaWsConnectTimeoutMs);
    cfg.reconnect_timeout_ms = 0;
    cfg.disable_auto_reconnect = true;
    cfg.keep_alive_enable = true;
    cfg.ping_interval_sec = static_cast<int>(kHaWsDefaultKeepAliveMs / 1000U);
    cfg.cert_pem = nullptr;
    cfg.use_global_ca_store = false;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    sHaWs.client = esp_websocket_client_init(&cfg);
    if (sHaWs.client == nullptr) {
      giveHaWsLock();
      reasonOut = "ws init failed";
      return false;
    }
    (void)esp_websocket_register_events(
        sHaWs.client, static_cast<esp_websocket_event_id_t>(-1), haWsEventHandler, nullptr);
  }

  if (!sHaWs.started && (sHaWs.reconnectDueMs == 0 || static_cast<int32_t>(nowMs - sHaWs.reconnectDueMs) >= 0)) {
    if (esp_websocket_client_start(sHaWs.client) == ESP_OK) {
      sHaWs.started = true;
      ESP_LOGI(kTag, "ha_ws connect widget=%s url=%s", widgetId.c_str(), sHaWs.wsUrl.c_str());
    } else {
      sHaWs.started = false;
      sHaWs.authOk = false;
      sHaWs.ready = false;
      if (sHaWs.failureStreak < 32) {
        ++sHaWs.failureStreak;
      }
      const uint32_t backoff = std::min<uint32_t>(60000U, 1000U << std::min<uint8_t>(sHaWs.failureStreak, 6));
      sHaWs.reconnectDueMs = nowMs + backoff;
      giveHaWsLock();
      reasonOut = "ws start failed";
      return false;
    }
  }

  const bool ready = sHaWs.ready;
  giveHaWsLock();
  if (!ready) {
    reasonOut = "ws not ready";
    return false;
  }
  return true;
}

bool readHaWsEntityJson(const std::string& entityId, std::string& outJson) {
  outJson.clear();
  if (entityId.empty()) {
    return false;
  }
  if (!takeHaWsLock(100)) {
    return false;
  }
  auto it = sHaWs.entityStateJson.find(entityId);
  if (it != sHaWs.entityStateJson.end()) {
    outJson = it->second;
  }
  giveHaWsLock();
  return !outJson.empty();
}

bool requestHaWsEntitySubscription(const std::string& entityId, std::string& reasonOut) {
  reasonOut.clear();
  if (entityId.empty()) {
    reasonOut = "ha_ws entity empty";
    return false;
  }
  if (!takeHaWsLock(150)) {
    reasonOut = "ws lock timeout";
    return false;
  }
  if (sHaWs.entityIdToTriggerSubId.find(entityId) != sHaWs.entityIdToTriggerSubId.end()) {
    giveHaWsLock();
    reasonOut = "ha_ws trigger subscribed";
    return true;
  }
  if (sHaWs.entityIdToTriggerReq.find(entityId) != sHaWs.entityIdToTriggerReq.end()) {
    giveHaWsLock();
    reasonOut = "ha_ws trigger pending";
    return true;
  }
  if (sHaWs.client == nullptr || !sHaWs.started || !sHaWs.authOk || !sHaWs.ready) {
    giveHaWsLock();
    reasonOut = "ws not ready";
    return false;
  }
  const uint32_t reqId = nextHaWsReqIdLocked();
  char prefix[192];
  std::snprintf(prefix, sizeof(prefix),
                "{\"id\":%u,\"type\":\"subscribe_trigger\",\"trigger\":[{\"platform\":\"state\",\"entity_id\":\"",
                static_cast<unsigned>(reqId));
  std::string msg(prefix);
  msg += jsonEscape(entityId);
  msg += "\"}]}";
  const int sent = esp_websocket_client_send_text(sHaWs.client, msg.c_str(), static_cast<int>(msg.size()),
                                                  pdMS_TO_TICKS(1000));
  if (sent < 0) {
    giveHaWsLock();
    reasonOut = "ha_ws trigger send failed";
    return false;
  }
  sHaWs.triggerReqToEntityId[reqId] = entityId;
  sHaWs.entityIdToTriggerReq[entityId] = reqId;
  giveHaWsLock();
  ESP_LOGI(kTag, "ha_ws trigger subscribe request entity=%s", entityId.c_str());
  reasonOut = "ha_ws trigger queued";
  return true;
}

bool requestHaWsEntityBootstrap(const std::string& entityId, std::string& reasonOut) {
  reasonOut.clear();
  if (entityId.empty()) {
    reasonOut = "ha_ws entity empty";
    return false;
  }
  if (!takeHaWsLock(150)) {
    reasonOut = "ws lock timeout";
    return false;
  }
  if (sHaWs.entityStateJson.find(entityId) != sHaWs.entityStateJson.end()) {
    giveHaWsLock();
    reasonOut = "ha_ws entity cached";
    return true;
  }
  if (sHaWs.entityIdToRenderReq.find(entityId) != sHaWs.entityIdToRenderReq.end()) {
    giveHaWsLock();
    reasonOut = "ha_ws bootstrap pending";
    return true;
  }
  if (sHaWs.client == nullptr || !sHaWs.started || !sHaWs.authOk || !sHaWs.ready) {
    giveHaWsLock();
    reasonOut = "ws not ready";
    return false;
  }
  const uint32_t reqId = nextHaWsReqIdLocked();
  const std::string templ =
      "{% set s = states[entity_id] %}"
      "{{ {'entity_id': entity_id,"
      "'state': (s.state if s else ''),"
      "'attributes': (s.attributes if s else {})} | tojson }}";
  char prefix[160];
  std::snprintf(prefix, sizeof(prefix),
                "{\"id\":%u,\"type\":\"render_template\",\"template\":\"",
                static_cast<unsigned>(reqId));
  std::string msg(prefix);
  msg += jsonEscape(templ);
  msg += "\",\"report_errors\":true,\"variables\":{\"entity_id\":\"";
  msg += jsonEscape(entityId);
  msg += "\"}}";
  const int sent = esp_websocket_client_send_text(sHaWs.client, msg.c_str(), static_cast<int>(msg.size()),
                                                  pdMS_TO_TICKS(1000));
  if (sent < 0) {
    giveHaWsLock();
    reasonOut = "ha_ws bootstrap send failed";
    return false;
  }
  sHaWs.renderReqToEntityId[reqId] = entityId;
  sHaWs.entityIdToRenderReq[entityId] = reqId;
  giveHaWsLock();
  ESP_LOGI(kTag, "ha_ws bootstrap request entity=%s", entityId.c_str());
  reasonOut = "ha_ws bootstrap queued";
  return true;
}

TapActionType parseTapActionTypeFromSettings(const std::map<std::string, std::string>& settings) {
  auto it = settings.find("tap_action");
  if (it == settings.end()) {
    return TapActionType::kNone;
  }
  std::string action = trimCopy(it->second);
  std::transform(action.begin(), action.end(), action.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (action == "refresh") {
    return TapActionType::kRefresh;
  }
  if (action == "http") {
    return TapActionType::kHttp;
  }
  if (action == "ha_ws" || action == "ha_ws_service" || action == "ws") {
    return TapActionType::kHaWsService;
  }
  return TapActionType::kNone;
}

void loadTapActionFromSettings() {
  s.tapAction = parseTapActionTypeFromSettings(s.settingValues);
  s.tapUrlTemplate.clear();
  s.tapMethod = "POST";
  s.tapBodyTemplate.clear();
  s.tapContentType = "application/json";
  s.tapHeaders.clear();
  if (s.tapAction != TapActionType::kHttp) {
    return;
  }

  if (auto it = s.settingValues.find("tap_url"); it != s.settingValues.end()) {
    s.tapUrlTemplate = it->second;
  }
  if (auto it = s.settingValues.find("tap_method"); it != s.settingValues.end()) {
    std::string m = trimCopy(it->second);
    std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) {
      return static_cast<char>(std::toupper(c));
    });
    if (!m.empty()) {
      s.tapMethod = m;
    }
  }
  if (auto it = s.settingValues.find("tap_body"); it != s.settingValues.end()) {
    s.tapBodyTemplate = it->second;
  }
  if (auto it = s.settingValues.find("tap_content_type"); it != s.settingValues.end()) {
    const std::string ctype = trimCopy(it->second);
    if (!ctype.empty()) {
      s.tapContentType = ctype;
    }
  }
  for (const auto& kv : s.settingValues) {
    if (kv.first.rfind("tap_header_", 0) != 0) {
      continue;
    }
    std::string name = kv.first.substr(std::strlen("tap_header_"));
    std::replace(name.begin(), name.end(), '_', '-');
    name = trimCopy(name);
    const std::string value = trimCopy(kv.second);
    if (!name.empty() && !value.empty()) {
      s.tapHeaders.push_back({name, value});
    }
  }

  // Preserve existing tap_url / tap_body config for HA cards while keeping HA traffic WS-only.
  if (s.source == DataSource::kHaWs && s.tapAction == TapActionType::kHttp) {
    s.tapAction = TapActionType::kHaWsService;
  }
}

bool httpRequestDirect(const std::string& method, const std::string& url,
                       const std::vector<KeyValue>& headers, const std::string& reqBody, int& statusCode,
                       std::string& body, std::string& reason, uint32_t& durationMs, std::string& hostOut,
                       bool& viaProxy, uint32_t maxResponseBytes) {
  statusCode = 0;
  body.clear();
  reason.clear();
  durationMs = 0;
  hostOut = hostFromUrl(url);
  viaProxy = isProxyUrl(url, hostOut);

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

  const uint32_t startMs = platform::millisMs();
  ESP_LOGD(kTag, "http start method=%s host=%s proxy=%d url=%s", method.c_str(), hostOut.c_str(),
           viaProxy ? 1 : 0, url.c_str());

  HttpCapture cap;
  cap.maxBytes = std::clamp<uint32_t>(maxResponseBytes, kHttpResponseMaxBytesMin, kHttpResponseMaxBytesMax);
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

  esp_http_client_method_t m = HTTP_METHOD_GET;
  if (method == "POST") {
    m = HTTP_METHOD_POST;
  } else if (method == "PUT") {
    m = HTTP_METHOD_PUT;
  } else if (method == "PATCH") {
    m = HTTP_METHOD_PATCH;
  } else if (method == "DELETE") {
    m = HTTP_METHOD_DELETE;
  } else if (method == "HEAD") {
    m = HTTP_METHOD_HEAD;
  }
  esp_http_client_set_method(client, m);
  esp_http_client_set_header(client, "Accept", "application/json");
  esp_http_client_set_header(client, "User-Agent", "CoStar-ESP32/1.0");
  esp_http_client_set_header(client, "Accept-Encoding", "identity");
  if (!reqBody.empty()) {
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, reqBody.c_str(), static_cast<int>(reqBody.size()));
  }
  for (const KeyValue& kv : headers) {
    const std::string key = trimCopy(kv.key);
    if (key.empty()) {
      continue;
    }
    const std::string value = bindRuntimeTemplate(kv.value);
    if (value.empty()) {
      continue;
    }
    esp_http_client_set_header(client, key.c_str(), value.c_str());
    std::string keyLower = key;
    std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    const bool sensitive = keyLower == "authorization" || keyLower == "cookie" ||
                           keyLower == "x-api-key" || keyLower == "proxy-authorization";
    std::string shown = value;
    if (sensitive) {
      if (shown.size() > 16) {
        shown = shown.substr(0, 8) + "...(" + std::to_string(static_cast<unsigned>(value.size())) +
                " bytes)";
      } else if (!shown.empty()) {
        shown = "***";
      }
    }
    ESP_LOGD(kTag, "http hdr host=%s key=%s value=%s", hostOut.c_str(), key.c_str(), shown.c_str());
  }

  const esp_err_t err = esp_http_client_perform(client);
  if (err == ESP_OK) {
    statusCode = esp_http_client_get_status_code(client);
  }
  body = std::move(cap.body);

  esp_http_client_cleanup(client);
  xSemaphoreGive(sHttpGate);
  durationMs = platform::millisMs() - startMs;

  if (err != ESP_OK) {
    reason = esp_err_to_name(err);
    ESP_LOGW(kTag, "http fail host=%s proxy=%d dur_ms=%u reason=%s", hostOut.c_str(), viaProxy ? 1 : 0,
             static_cast<unsigned>(durationMs), reason.c_str());
    return false;
  }
  if (statusCode <= 0) {
    reason = "no-http-status";
    ESP_LOGW(kTag, "http fail host=%s proxy=%d dur_ms=%u reason=%s", hostOut.c_str(), viaProxy ? 1 : 0,
             static_cast<unsigned>(durationMs), reason.c_str());
    return false;
  }
  if (cap.overflow) {
    reason = "http body too large";
    ESP_LOGW(kTag, "http fail host=%s proxy=%d dur_ms=%u reason=%s max_bytes=%u", hostOut.c_str(),
             viaProxy ? 1 : 0, static_cast<unsigned>(durationMs), reason.c_str(),
             static_cast<unsigned>(cap.maxBytes));
    return false;
  }

  ESP_LOGD(kTag, "http done method=%s host=%s proxy=%d status=%d bytes=%u dur_ms=%u", method.c_str(),
           hostOut.c_str(),
           viaProxy ? 1 : 0, statusCode, static_cast<unsigned>(body.size()),
           static_cast<unsigned>(durationMs));
  return true;
}

void httpWorkerTask(void* /*arg*/) {
  for (;;) {
    HttpJob* job = nullptr;
    if (xQueueReceive(sHttpJobQueue, &job, portMAX_DELAY) != pdTRUE || job == nullptr) {
      continue;
    }
    HttpResult* result = new HttpResult();
    if (result == nullptr) {
      delete job;
      continue;
    }
    result->ok =
        httpRequestDirect(job->method, job->url, job->headers, job->body, result->statusCode, result->body,
                          result->reason, result->durationMs, result->host, result->viaProxy,
                          job->maxResponseBytes);
    if (job->replyQueue != nullptr) {
      if (xQueueSend(job->replyQueue, &result, pdMS_TO_TICKS(100)) != pdTRUE) {
        delete result;
      }
    } else {
      delete result;
    }
    delete job;
  }
}

bool ensureHttpWorker() {
  if (sHttpJobQueue == nullptr) {
    sHttpJobQueue = xQueueCreate(kHttpWorkerQueueLen, sizeof(HttpJob*));
    if (sHttpJobQueue == nullptr) {
      return false;
    }
  }
  if (sHttpWorkerTask == nullptr) {
    if (xTaskCreatePinnedToCore(httpWorkerTask, "dsl-http", kHttpWorkerStack, nullptr, kHttpWorkerPriority,
                                &sHttpWorkerTask, kHttpWorkerCore) != pdPASS) {
      sHttpWorkerTask = nullptr;
      return false;
    }
  }
  return true;
}

bool httpGet(const std::string& url, const std::vector<KeyValue>& headers, int& statusCode, std::string& body,
             std::string& reason) {
  statusCode = 0;
  body.clear();
  reason.clear();

  if (!ensureHttpWorker()) {
    uint32_t durationMs = 0;
    std::string host;
    bool viaProxy = false;
    return httpRequestDirect("GET", url, headers, "", statusCode, body, reason, durationMs, host, viaProxy,
                             s.httpMaxBytes);
  }

  QueueHandle_t replyQueue = xQueueCreate(1, sizeof(HttpResult*));
  if (replyQueue == nullptr) {
    reason = "http reply queue alloc failed";
    return false;
  }
  HttpJob* job = new HttpJob();
  if (job == nullptr) {
    vQueueDelete(replyQueue);
    reason = "http job alloc failed";
    return false;
  }
  job->url = url;
  job->method = "GET";
  job->body.clear();
  job->headers = headers;
  job->maxResponseBytes = s.httpMaxBytes;
  job->replyQueue = replyQueue;

  if (xQueueSend(sHttpJobQueue, &job, pdMS_TO_TICKS(kHttpGateTimeoutMs)) != pdTRUE) {
    delete job;
    vQueueDelete(replyQueue);
    reason = "http worker queue full";
    return false;
  }

  HttpResult* result = nullptr;
  const BaseType_t got = xQueueReceive(replyQueue, &result, pdMS_TO_TICKS(kHttpWorkerReplyTimeoutMs));
  vQueueDelete(replyQueue);
  if (got != pdTRUE || result == nullptr) {
    reason = "http worker timeout";
    return false;
  }

  statusCode = result->statusCode;
  body = std::move(result->body);
  reason = std::move(result->reason);
  const bool ok = result->ok;
  delete result;
  return ok;
}

bool httpRequest(const std::string& method, const std::string& url, const std::vector<KeyValue>& headers,
                 const std::string& reqBody, int& statusCode, std::string& body, std::string& reason) {
  statusCode = 0;
  body.clear();
  reason.clear();

  if (!ensureHttpWorker()) {
    uint32_t durationMs = 0;
    std::string host;
    bool viaProxy = false;
    return httpRequestDirect(method, url, headers, reqBody, statusCode, body, reason, durationMs, host,
                             viaProxy, s.httpMaxBytes);
  }

  QueueHandle_t replyQueue = xQueueCreate(1, sizeof(HttpResult*));
  if (replyQueue == nullptr) {
    reason = "http reply queue alloc failed";
    return false;
  }
  HttpJob* job = new HttpJob();
  if (job == nullptr) {
    vQueueDelete(replyQueue);
    reason = "http job alloc failed";
    return false;
  }
  job->method = method;
  job->url = url;
  job->body = reqBody;
  job->headers = headers;
  job->maxResponseBytes = s.httpMaxBytes;
  job->replyQueue = replyQueue;

  if (xQueueSend(sHttpJobQueue, &job, pdMS_TO_TICKS(kHttpGateTimeoutMs)) != pdTRUE) {
    delete job;
    vQueueDelete(replyQueue);
    reason = "http worker queue full";
    return false;
  }

  HttpResult* result = nullptr;
  const BaseType_t got = xQueueReceive(replyQueue, &result, pdMS_TO_TICKS(kHttpWorkerReplyTimeoutMs));
  vQueueDelete(replyQueue);
  if (got != pdTRUE || result == nullptr) {
    reason = "http worker timeout";
    return false;
  }
  statusCode = result->statusCode;
  body = std::move(result->body);
  reason = std::move(result->reason);
  const bool ok = result->ok;
  delete result;
  return ok;
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

void noteFetchDeferred(const char* reason) {
  const uint32_t nowMs = platform::millisMs();
  if (s.backoffUntilMs == 0 || static_cast<int32_t>(s.backoffUntilMs - nowMs) > 250) {
    s.backoffUntilMs = nowMs + 250;
  }
  ESP_LOGI(kTag, "fetch deferred widget=%s reason=%s", s.widgetId.c_str(), reason != nullptr ? reason : "unknown");
}

void noteFetchSuccess() {
  s.failureStreak = 0;
  s.backoffUntilMs = 0;
}

bool executeTapAction(std::string& reasonOut) {
  reasonOut.clear();
  if (s.tapAction == TapActionType::kNone) {
    reasonOut = "tap action none";
    return false;
  }
  if (s.tapAction == TapActionType::kRefresh) {
    s.lastFetchMs = 0;
    s.backoffUntilMs = 0;
    return true;
  }

  if (s.tapAction == TapActionType::kHaWsService) {
    const std::string url = bindRuntimeTemplate(s.tapUrlTemplate);
    std::string domain;
    std::string service;
    if (!parseHaServiceFromUrl(url, domain, service)) {
      reasonOut = "ha_ws tap_url invalid";
      return false;
    }
    const std::string body = bindRuntimeTemplate(s.tapBodyTemplate);
    if (!haWsCallService(domain, service, body, reasonOut)) {
      return false;
    }
    return true;
  }

  if (s.tapAction != TapActionType::kHttp) {
    reasonOut = "unsupported tap action";
    return false;
  }

  const std::string url = bindRuntimeTemplate(s.tapUrlTemplate);
  if (url.empty()) {
    reasonOut = "tap_url empty";
    return false;
  }
  std::string method = s.tapMethod.empty() ? "POST" : s.tapMethod;
  std::transform(method.begin(), method.end(), method.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  const std::string body = bindRuntimeTemplate(s.tapBodyTemplate);

  std::vector<KeyValue> headers;
  headers.reserve(s.tapHeaders.size());
  for (const auto& kv : s.tapHeaders) {
    const std::string key = trimCopy(kv.key);
    const std::string value = bindRuntimeTemplate(kv.value);
    if (!key.empty() && !value.empty()) {
      headers.push_back({key, value});
    }
  }
  if (!body.empty()) {
    headers.push_back({"Content-Type", s.tapContentType.empty() ? "application/json" : s.tapContentType});
  }

  int status = 0;
  std::string resp;
  std::string reason;
  if (!httpRequest(method, url, headers, body, status, resp, reason)) {
    reasonOut = reason;
    return false;
  }
  if (status < 200 || status >= 300) {
    reasonOut = "status=" + std::to_string(status);
    return false;
  }
  s.tapRefreshDueMs = platform::millisMs() + kTapPostHttpRefreshDelayMs;
  ESP_LOGI(kTag, "tap scheduled refresh widget=%s delay_ms=%u", s.widgetId.c_str(),
           static_cast<unsigned>(kTapPostHttpRefreshDelayMs));
  return true;
}

bool resolveFieldsFromJsonView(std::string_view jsonText) {
  int resolved = 0;
  int missing = 0;
  s.numericValues.clear();
  applyTransforms(jsonText);

  for (const FieldSpec& field : s.fields) {
    std::string path = bindRuntimeTemplate(field.path);
    std::string raw;
    bool numeric = false;
    double numericValue = 0.0;

    if (path == "computed.moon_phase") {
      if (!computeMoonPhaseName(raw)) {
        ++missing;
        (void)setValue(field.key, "");
        s.numericValues.erase(field.key);
        continue;
      }
    } else {
      std::string_view valueView;
      if (!resolveJsonPath(jsonText, path, valueView)) {
        auto tIt = s.transformValues.find(path);
        if (tIt != s.transformValues.end()) {
          raw = tIt->second;
          numeric = parseStrictDouble(raw, numericValue);
        } else {
          ++missing;
          if (s.debug) {
            ESP_LOGW(kTag, "field miss key=%s path=%s", field.key.c_str(), path.c_str());
          }
          (void)setValue(field.key, "");
          s.numericValues.erase(field.key);
          continue;
        }
      } else {
        raw = valueViewToText(valueView);
        numeric = valueViewToNumeric(valueView, numericValue);
      }
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
    if (numeric) {
      s.numericValues[field.key] = numericValue;
    } else {
      s.numericValues.erase(field.key);
    }
    ++resolved;
  }

  applyWeatherDerivedValues();

  if (s.debug) {
    ESP_LOGI(kTag, "parse summary resolved=%d missing=%d total=%u", resolved, missing,
             static_cast<unsigned>(s.fields.size()));
  }

  return resolved > 0;
}

bool resolveFieldsFromHttp(std::string&& jsonTextOwned) {
  if (s.retainSourceJson) {
    s.sourceJson = std::move(jsonTextOwned);
    return resolveFieldsFromJsonView(std::string_view(s.sourceJson));
  }
  s.sourceJson.clear();
  return resolveFieldsFromJsonView(std::string_view(jsonTextOwned));
}

bool resolveFieldsFromLocalTime() {
  LocalTimeContext ctx;
  if (!buildLocalTimeContext(ctx)) {
    return false;
  }

  int resolved = 0;
  int missing = 0;
  s.numericValues.clear();
  s.sourceJson.clear();
  for (const FieldSpec& field : s.fields) {
    const std::string path = bindRuntimeTemplate(field.path);

    std::string raw;
    bool numeric = false;
    double numericValue = 0.0;
    if (!resolveLocalTimeValue(ctx, path, raw, numeric, numericValue)) {
      ++missing;
      (void)setValue(field.key, "");
      s.numericValues.erase(field.key);
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
    if (numeric) {
      s.numericValues[field.key] = numericValue;
    } else {
      s.numericValues.erase(field.key);
    }
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

  if (s.source == DataSource::kHaWs) {
    const std::string entityId = bindRuntimeTemplate(s.wsEntityTemplate.empty() ? "{{setting.entity_id}}"
                                                                                 : s.wsEntityTemplate);
    const std::string token = bindRuntimeTemplate(readSetting("ha_token", ""));
    const std::string wsBase = bindRuntimeTemplate(readSetting("ha_ws_url", ""));
    const std::string baseUrl = bindRuntimeTemplate(readSetting("ha_base_url", ""));
    const std::string wsPath = bindRuntimeTemplate(readSetting("ha_ws_path", "/api/websocket"));
    std::string wsUrl;
    if (wsBase.empty()) {
      (void)normalizeHaWsUrl(baseUrl, wsPath, wsUrl);
    } else {
      (void)normalizeHaWsUrl(wsBase, wsPath, wsUrl);
    }
    std::string wsReason;
    const bool wsReady = ensureHaWsConnected(wsUrl, token, s.widgetId, wsReason);
    std::string entityJson;
    if (!readHaWsEntityJson(entityId, entityJson)) {
      if (!wsReady) {
        noteFetchDeferred(wsReason.c_str());
      } else {
        std::string ignoredTriggerReason;
        (void)requestHaWsEntitySubscription(entityId, ignoredTriggerReason);
        std::string bootstrapReason;
        (void)requestHaWsEntityBootstrap(entityId, bootstrapReason);
        noteFetchDeferred(bootstrapReason.c_str());
      }
      return false;
    }
    if (!resolveFieldsFromHttp(std::move(entityJson))) {
      noteFetchFailure(nowMs, "ha_ws parse unresolved");
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
  std::vector<KeyValue> resolvedHeaders;
  resolvedHeaders.reserve(s.headers.size());
  for (const KeyValue& kv : s.headers) {
    const std::string key = trimCopy(kv.key);
    if (key.empty()) {
      continue;
    }
    resolvedHeaders.push_back({key, bindRuntimeTemplate(kv.value)});
  }
  int statusCode = 0;
  std::string body;
  std::string reason;
  if (!httpGet(url, resolvedHeaders, statusCode, body, reason)) {
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

  if (!resolveFieldsFromHttp(std::move(body))) {
    noteFetchFailure(nowMs, "dsl parse unresolved");
    return false;
  }

  noteFetchSuccess();
  return true;
}

bool inWidgetBounds(int x, int y) {
  return x >= static_cast<int>(s.x) && y >= static_cast<int>(s.y) &&
         x < static_cast<int>(s.x + s.w) && y < static_cast<int>(s.y + s.h);
}

inline bool canvasActive() { return sCanvas != nullptr && sCanvasW == s.w && sCanvasH > 0; }

inline bool canvasCovers(int x, int y) {
  if (!canvasActive()) {
    return false;
  }
  const int x0 = static_cast<int>(s.x);
  const int x1 = static_cast<int>(s.x + sCanvasW);
  const int y0 = static_cast<int>(sCanvasY0);
  const int y1 = static_cast<int>(sCanvasY0 + sCanvasH);
  return x >= x0 && x < x1 && y >= y0 && y < y1;
}

void drawSolidRect(int x, int y, int w, int h, uint16_t color) {
  if (w <= 0 || h <= 0) {
    return;
  }
  int x0 = std::max(x, static_cast<int>(s.x));
  int y0 = std::max(y, static_cast<int>(s.y));
  int x1 = std::min(x + w, static_cast<int>(s.x + s.w));
  int y1 = std::min(y + h, static_cast<int>(s.y + s.h));
  if (x1 <= x0 || y1 <= y0) {
    return;
  }

  if (canvasActive()) {
    x0 = std::max(x0, static_cast<int>(s.x));
    x1 = std::min(x1, static_cast<int>(s.x + sCanvasW));
    y0 = std::max(y0, static_cast<int>(sCanvasY0));
    y1 = std::min(y1, static_cast<int>(sCanvasY0 + sCanvasH));
    if (x1 <= x0 || y1 <= y0) {
      return;
    }
    const int localX = x0 - static_cast<int>(s.x);
    const int localY = y0 - static_cast<int>(sCanvasY0);
    const int span = x1 - x0;
    for (int row = 0; row < (y1 - y0); ++row) {
      uint16_t* dst =
          sCanvas + static_cast<size_t>(localY + row) * static_cast<size_t>(sCanvasW) + localX;
      std::fill(dst, dst + span, color);
    }
    return;
  }

  (void)display_spi::fillRect(static_cast<uint16_t>(x0), static_cast<uint16_t>(y0),
                              static_cast<uint16_t>(x1 - x0), static_cast<uint16_t>(y1 - y0), color);
}

void drawPixel(int x, int y, uint16_t color) {
  if (!inWidgetBounds(x, y)) {
    return;
  }
  if (canvasActive()) {
    if (!canvasCovers(x, y)) {
      return;
    }
    const int localX = x - static_cast<int>(s.x);
    const int localY = y - static_cast<int>(sCanvasY0);
    sCanvas[static_cast<size_t>(localY) * static_cast<size_t>(sCanvasW) + static_cast<size_t>(localX)] =
        color;
    return;
  }
  (void)display_spi::fillRect(static_cast<uint16_t>(x), static_cast<uint16_t>(y), 1, 1, color);
}

void drawGlyph(int x, int y, char ch, uint16_t fg, uint16_t bg, int scale) {
  const uint8_t c = static_cast<uint8_t>(ch);
  const size_t base = static_cast<size_t>(c) * 5U;
  for (int col = 0; col < 5; ++col) {
    const uint8_t bits = font[base + static_cast<size_t>(col)];
    for (int row = 0; row < 7; ++row) {
      const bool on = ((bits >> row) & 1U) != 0;
      const uint16_t color = on ? fg : bg;
      for (int sx = 0; sx < scale; ++sx) {
        for (int sy = 0; sy < scale; ++sy) {
          drawPixel(x + col * scale + sx, y + row * scale + sy, color);
        }
      }
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

int textWidthPx(const std::string& text, int scale) {
  return static_cast<int>(text.size()) * 6 * std::max(scale, 1);
}

std::string ellipsizeToWidth(const std::string& text, int scale, int maxWidthPx) {
  if (maxWidthPx <= 0 || textWidthPx(text, scale) <= maxWidthPx) {
    return text;
  }
  const std::string dots = "...";
  if (textWidthPx(dots, scale) > maxWidthPx) {
    return {};
  }
  std::string out = text;
  while (!out.empty() && textWidthPx(out + dots, scale) > maxWidthPx) {
    out.pop_back();
  }
  return out + dots;
}

std::vector<std::string> wrapLabelLines(const std::string& text, int scale, int maxWidthPx) {
  std::vector<std::string> lines;
  if (maxWidthPx <= 0) {
    lines.push_back(text);
    return lines;
  }

  std::string line;
  std::string word;
  auto flushWord = [&]() {
    if (word.empty()) {
      return;
    }
    if (line.empty()) {
      if (textWidthPx(word, scale) <= maxWidthPx) {
        line = word;
      } else {
        std::string part;
        for (char c : word) {
          if (textWidthPx(part + c, scale) <= maxWidthPx) {
            part.push_back(c);
          } else {
            if (!part.empty()) {
              lines.push_back(part);
              part.clear();
            }
            part.push_back(c);
          }
        }
        line = part;
      }
    } else {
      const std::string candidate = line + " " + word;
      if (textWidthPx(candidate, scale) <= maxWidthPx) {
        line = candidate;
      } else {
        lines.push_back(line);
        line = word;
      }
    }
    word.clear();
  };

  for (char c : text) {
    if (c == '\n') {
      flushWord();
      lines.push_back(line);
      line.clear();
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      flushWord();
      continue;
    }
    word.push_back(c);
  }
  flushWord();
  if (!line.empty() || lines.empty()) {
    lines.push_back(line);
  }
  return lines;
}

void drawLine(int x0, int y0, int x1, int y1, uint16_t color) {
  int dx = std::abs(x1 - x0);
  int sx = x0 < x1 ? 1 : -1;
  int dy = -std::abs(y1 - y0);
  int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  while (true) {
    drawPixel(x0, y0, color);
    if (x0 == x1 && y0 == y1) {
      break;
    }
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void drawCircle(int cx, int cy, int r, uint16_t color, bool fill) {
  if (r <= 0) {
    return;
  }
  if (fill) {
    for (int y = -r; y <= r; ++y) {
      const int xx = static_cast<int>(std::sqrt(std::max(0, r * r - y * y)));
      for (int x = -xx; x <= xx; ++x) {
        drawPixel(cx + x, cy + y, color);
      }
    }
    return;
  }
  int x = r;
  int y = 0;
  int err = 1 - x;
  while (x >= y) {
    drawPixel(cx + x, cy + y, color);
    drawPixel(cx + y, cy + x, color);
    drawPixel(cx - y, cy + x, color);
    drawPixel(cx - x, cy + y, color);
    drawPixel(cx - x, cy - y, color);
    drawPixel(cx - y, cy - x, color);
    drawPixel(cx + y, cy - x, color);
    drawPixel(cx + x, cy - y, color);
    ++y;
    if (err < 0) {
      err += 2 * y + 1;
    } else {
      --x;
      err += 2 * (y - x) + 1;
    }
  }
}

bool loadIcon(const std::string& path, int w, int h, std::vector<uint16_t>& outPixels) {
  if (path.empty() || w <= 0 || h <= 0) {
    return false;
  }
  std::string fullPath = path;
  if (fullPath.rfind("/littlefs/", 0) != 0) {
    if (!fullPath.empty() && fullPath.front() == '/') {
      fullPath = "/littlefs" + fullPath;
    } else {
      fullPath = "/littlefs/" + fullPath;
    }
  }
  std::FILE* fp = std::fopen(fullPath.c_str(), "rb");
  if (fp == nullptr) {
    return false;
  }
  const size_t needPixels = static_cast<size_t>(w) * static_cast<size_t>(h);
  outPixels.assign(needPixels, 0);
  const size_t got = std::fread(outPixels.data(), sizeof(uint16_t), needPixels, fp);
  std::fclose(fp);
  return got == needPixels;
}

bool isHttpUrl(const std::string& path) {
  return path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0;
}

uint64_t fnv1a64(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= static_cast<uint64_t>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

std::string hex64(uint64_t v) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<size_t>(i)] = kHex[static_cast<size_t>(v & 0x0FULL)];
    v >>= 4;
  }
  return out;
}

std::string iconCacheKey(const std::string& url, int w, int h) {
  return url + "|" + std::to_string(w) + "x" + std::to_string(h);
}

std::string iconCacheFilePath(const std::string& cacheKey) {
  return std::string(kIconCacheDir) + "/" + hex64(fnv1a64(cacheKey)) + ".raw";
}

bool ensureIconCacheDir() {
  if (sIconCacheDirReady) {
    return true;
  }
  if (::mkdir(kIconCacheDir, 0777) == 0 || errno == EEXIST) {
    sIconCacheDirReady = true;
    return true;
  }
  ESP_LOGW(kTag, "icon cache dir create failed path=%s errno=%d", kIconCacheDir, errno);
  return false;
}

bool iconMemCacheGet(const std::string& key, std::vector<uint16_t>& outPixels) {
  auto it = sIconMemCache.find(key);
  if (it == sIconMemCache.end()) {
    return false;
  }
  it->second.lastUsedMs = platform::millisMs();
  outPixels = it->second.pixels;
  return true;
}

void iconMemCachePut(const std::string& key, const std::vector<uint16_t>& pixels) {
  const size_t bytes = pixels.size() * sizeof(uint16_t);
  if (bytes > kIconMemCacheBudgetBytes) {
    return;
  }

  auto existing = sIconMemCache.find(key);
  if (existing != sIconMemCache.end()) {
    sIconMemCacheBytes -= existing->second.pixels.size() * sizeof(uint16_t);
    sIconMemCache.erase(existing);
  }

  while (sIconMemCacheBytes + bytes > kIconMemCacheBudgetBytes && !sIconMemCache.empty()) {
    auto victim = sIconMemCache.begin();
    for (auto it = sIconMemCache.begin(); it != sIconMemCache.end(); ++it) {
      if (it->second.lastUsedMs < victim->second.lastUsedMs) {
        victim = it;
      }
    }
    sIconMemCacheBytes -= victim->second.pixels.size() * sizeof(uint16_t);
    sIconMemCache.erase(victim);
  }

  IconMemEntry entry;
  entry.pixels = pixels;
  entry.lastUsedMs = platform::millisMs();
  sIconMemCacheBytes += bytes;
  sIconMemCache[key] = std::move(entry);
}

bool iconFileCacheGet(const std::string& key, int w, int h, std::vector<uint16_t>& outPixels) {
  if (!ensureIconCacheDir()) {
    return false;
  }
  const std::string path = iconCacheFilePath(key);
  std::FILE* fp = std::fopen(path.c_str(), "rb");
  if (fp == nullptr) {
    return false;
  }
  const size_t needPixels = static_cast<size_t>(w) * static_cast<size_t>(h);
  outPixels.assign(needPixels, 0);
  const size_t got = std::fread(outPixels.data(), sizeof(uint16_t), needPixels, fp);
  std::fclose(fp);
  return got == needPixels;
}

void iconFileCachePut(const std::string& key, const std::vector<uint16_t>& pixels) {
  if (!ensureIconCacheDir()) {
    return;
  }
  const std::string path = iconCacheFilePath(key);
  std::FILE* fp = std::fopen(path.c_str(), "wb");
  if (fp == nullptr) {
    return;
  }
  (void)std::fwrite(pixels.data(), sizeof(uint16_t), pixels.size(), fp);
  std::fclose(fp);
}

bool loadIconRemote(const std::string& url, int w, int h, std::vector<uint16_t>& outPixels) {
  if (url.empty() || w <= 0 || h <= 0) {
    return false;
  }
  const std::string key = iconCacheKey(url, w, h);
  const uint32_t nowMs = platform::millisMs();
  auto retryIt = sIconRetryAfterMs.find(key);
  if (retryIt != sIconRetryAfterMs.end() && nowMs < retryIt->second) {
    return false;
  }
  if (iconMemCacheGet(key, outPixels)) {
    sIconRetryAfterMs.erase(key);
    return true;
  }
  if (iconFileCacheGet(key, w, h, outPixels)) {
    iconMemCachePut(key, outPixels);
    sIconRetryAfterMs.erase(key);
    return true;
  }

  int status = 0;
  std::string body;
  std::string reason;
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (largest < 8192U) {
    sIconRetryAfterMs[key] = nowMs + kIconFetchRetryMs;
    ESP_LOGW(kTag, "icon fetch skipped low_heap url=%s largest=%u", url.c_str(), static_cast<unsigned>(largest));
    return false;
  }
  if (!httpGet(url, {}, status, body, reason)) {
    ESP_LOGW(kTag, "icon fetch fail url=%s reason=%s", url.c_str(), reason.c_str());
    sIconRetryAfterMs[key] = nowMs + kIconFetchRetryMs;
    return false;
  }
  if (status < 200 || status >= 300) {
    ESP_LOGW(kTag, "icon fetch status=%d url=%s", status, url.c_str());
    sIconRetryAfterMs[key] = nowMs + kIconFetchRetryMs;
    return false;
  }
  const size_t needBytes = static_cast<size_t>(w) * static_cast<size_t>(h) * sizeof(uint16_t);
  if (body.size() != needBytes) {
    ESP_LOGW(kTag, "icon fetch size mismatch url=%s got=%u expect=%u", url.c_str(),
             static_cast<unsigned>(body.size()), static_cast<unsigned>(needBytes));
    sIconRetryAfterMs[key] = nowMs + kIconFetchRetryMs;
    return false;
  }
  const size_t needPixels = static_cast<size_t>(w) * static_cast<size_t>(h);
  outPixels.resize(needPixels);
  std::memcpy(outPixels.data(), body.data(), needBytes);
  iconMemCachePut(key, outPixels);
  iconFileCachePut(key, outPixels);
  sIconRetryAfterMs.erase(key);
  return true;
}

bool getNumeric(const std::string& key, float& out) {
  auto it = s.numericValues.find(key);
  if (it != s.numericValues.end()) {
    out = static_cast<float>(it->second);
    return true;
  }
  const std::string* value = getValue(key);
  if (value == nullptr) {
    return false;
  }
  double parsed = 0.0;
  if (!parseStrictDouble(*value, parsed)) {
    return false;
  }
  out = static_cast<float>(parsed);
  return true;
}

bool datumIsCenter(TextDatum datum) {
  return datum == TextDatum::kTC || datum == TextDatum::kMC || datum == TextDatum::kBC ||
         datum == TextDatum::kCBaseline;
}

bool datumIsRight(TextDatum datum) {
  return datum == TextDatum::kTR || datum == TextDatum::kMR || datum == TextDatum::kBR ||
         datum == TextDatum::kRBaseline;
}

bool datumIsMiddle(TextDatum datum) {
  return datum == TextDatum::kML || datum == TextDatum::kMC || datum == TextDatum::kMR;
}

bool datumIsBottom(TextDatum datum) {
  return datum == TextDatum::kBL || datum == TextDatum::kBC || datum == TextDatum::kBR;
}

bool datumIsBaseline(TextDatum datum) {
  return datum == TextDatum::kLBaseline || datum == TextDatum::kCBaseline || datum == TextDatum::kRBaseline;
}

TextDatum topLineDatum(TextDatum datum) {
  if (datumIsCenter(datum)) {
    return TextDatum::kTC;
  }
  if (datumIsRight(datum)) {
    return TextDatum::kTR;
  }
  return TextDatum::kTL;
}

int datumTextX(int x, int textW, TextDatum datum) {
  if (datumIsCenter(datum)) {
    return x - (textW / 2);
  }
  if (datumIsRight(datum)) {
    return x - textW;
  }
  return x;
}

int datumTextY(int y, int textH, int scale, TextDatum datum) {
  if (datumIsMiddle(datum)) {
    return y - (textH / 2);
  }
  if (datumIsBottom(datum)) {
    return y - textH;
  }
  if (datumIsBaseline(datum)) {
    return y - std::max(1, textH - scale);
  }
  return y;
}

void renderNodes() {
  for (Node& node : s.nodes) {
    const int x = static_cast<int>(s.x) + node.x;
    const int y = static_cast<int>(s.y) + node.y;
    const int scale = std::max(1, std::min(3, node.font <= 1 ? 1 : (node.font >= 4 ? 2 : 1)));

    if (node.type == NodeType::kLabel) {
      std::string text = bindRuntimeTemplate(node.text);
      if (!node.path.empty()) {
        std::string value = "";
        if (const std::string* v = getValue(node.path); v != nullptr) {
          value = *v;
        } else if (!s.sourceJson.empty()) {
          std::string_view pathValue;
          if (resolveJsonPath(std::string_view(s.sourceJson), node.path, pathValue)) {
            value = valueViewToText(pathValue);
          }
        }
        if (node.text.empty()) {
          text = value;
        } else {
          text = replaceAll(text, "{{value}}", value);
        }
      }
      if (!node.wrap || node.w <= 0) {
        const int textW = textWidthPx(text, scale);
        const int textH = 8 * scale;
        const int textX = datumTextX(x, textW, node.datum);
        const int textY = datumTextY(y, textH, scale, node.datum);
        drawText(textX, textY, text, node.color565, kBg, scale);
        continue;
      }

      int lineHeight = node.lineHeight > 0 ? node.lineHeight : (8 * scale);
      if (lineHeight <= 0) {
        lineHeight = 8;
      }
      int maxLines = node.maxLines > 0 ? node.maxLines : 0;
      if (node.h > 0) {
        const int byHeight = node.h / lineHeight;
        if (byHeight > 0) {
          maxLines = maxLines > 0 ? std::min(maxLines, byHeight) : byHeight;
        }
      }

      std::vector<std::string> lines = wrapLabelLines(text, scale, node.w);
      bool truncated = false;
      if (maxLines > 0 && static_cast<int>(lines.size()) > maxLines) {
        lines.resize(static_cast<size_t>(maxLines));
        truncated = true;
      }
      if (truncated && !lines.empty() && node.overflow == OverflowMode::kEllipsis) {
        lines.back() = ellipsizeToWidth(lines.back(), scale, node.w);
      }

      const int blockH = static_cast<int>(lines.size()) * lineHeight;
      int startY = y;
      if (datumIsMiddle(node.datum)) {
        startY = y - (blockH / 2);
      } else if (datumIsBottom(node.datum)) {
        startY = y - blockH;
      }
      const TextDatum lineDatum = topLineDatum(node.datum);
      for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].empty()) {
          continue;
        }
        const int lineW = textWidthPx(lines[i], scale);
        const int lineX = datumTextX(x, lineW, lineDatum);
        drawText(lineX, startY + static_cast<int>(i) * lineHeight, lines[i], node.color565, kBg, scale);
      }
      continue;
    }

    if (node.type == NodeType::kValueBox) {
      drawSolidRect(x, y, node.w, node.h, node.bg565);
      std::string caption = bindRuntimeTemplate(node.text);
      if (!caption.empty()) {
        drawText(x + 4, y + 4, caption, node.color565, node.bg565, 1);
      }
      std::string value = node.key.empty() ? std::string() : (getValue(node.key) ? *getValue(node.key) : "");
      drawText(x + 4, y + 16, value, node.color565, node.bg565, scale);
      continue;
    }

    if (node.type == NodeType::kProgress) {
      drawSolidRect(x, y, node.w, node.h, node.bg565);
      float value = 0.0f;
      if (!node.key.empty() && getNumeric(node.key, value) && node.max > node.min && node.w > 4 &&
          node.h > 4) {
        float ratio = (value - node.min) / (node.max - node.min);
        ratio = std::clamp(ratio, 0.0f, 1.0f);
        const int fillW = static_cast<int>((node.w - 4) * ratio);
        drawSolidRect(x + 2, y + 2, fillW, node.h - 4, node.color565);
      }
      continue;
    }

    if (node.type == NodeType::kSparkline) {
      float value = 0.0f;
      if (!node.key.empty() && getNumeric(node.key, value)) {
        node.sparkValues.push_back(value);
        if (node.sparkValues.size() > static_cast<size_t>(std::max(8, node.w - 2))) {
          node.sparkValues.erase(node.sparkValues.begin());
        }
      }
      if (node.sparkValues.size() >= 2 && node.w > 2 && node.h > 2) {
        float minV = node.min;
        float maxV = node.max;
        if (maxV <= minV) {
          minV = node.sparkValues.front();
          maxV = node.sparkValues.front();
          for (float v : node.sparkValues) {
            minV = std::min(minV, v);
            maxV = std::max(maxV, v);
          }
          if (std::fabs(maxV - minV) < 0.001f) {
            maxV = minV + 1.0f;
          }
        }
        for (size_t i = 1; i < node.sparkValues.size(); ++i) {
          const float x0f = static_cast<float>(i - 1) / static_cast<float>(node.sparkValues.size() - 1);
          const float x1f = static_cast<float>(i) / static_cast<float>(node.sparkValues.size() - 1);
          const float y0f = (node.sparkValues[i - 1] - minV) / (maxV - minV);
          const float y1f = (node.sparkValues[i] - minV) / (maxV - minV);
          drawLine(x + 1 + static_cast<int>(x0f * (node.w - 2)),
                   y + node.h - 2 - static_cast<int>(y0f * (node.h - 2)),
                   x + 1 + static_cast<int>(x1f * (node.w - 2)),
                   y + node.h - 2 - static_cast<int>(y1f * (node.h - 2)), node.color565);
        }
      }
      continue;
    }

    if (node.type == NodeType::kArc) {
      const int r = node.radius > 0 ? node.radius : node.w / 2;
      if (r <= 0) {
        continue;
      }
      if (std::fabs(node.endDeg - node.startDeg) >= 359.0f && node.bg565 != kBg) {
        drawCircle(x, y, r, node.bg565, true);
      }
      const int thickness = std::max(1, node.thickness);
      const float step = std::fabs(node.endDeg - node.startDeg) > 120.0f ? 2.0f : 1.0f;
      for (int t = 0; t < thickness; ++t) {
        const int rr = r - t;
        for (float a = node.startDeg; a <= node.endDeg; a += step) {
          const float rad = (a - 90.0f) * (3.14159265f / 180.0f);
          drawPixel(x + static_cast<int>(std::cos(rad) * rr), y + static_cast<int>(std::sin(rad) * rr),
                    node.color565);
        }
      }
      continue;
    }

    if (node.type == NodeType::kLine) {
      float angleDeg = 0.0f;
      bool useAngle = false;
      if (!node.angleExpr.empty()) {
        useAngle = evalNumericExpr(bindRuntimeTemplate(node.angleExpr), nullptr, angleDeg);
      } else if (!node.key.empty()) {
        useAngle = getNumeric(node.key, angleDeg);
      }
      int x2 = static_cast<int>(s.x) + node.x2;
      int y2 = static_cast<int>(s.y) + node.y2;
      if (useAngle) {
        const int length = node.length > 0 ? node.length : node.radius;
        if (length <= 0) {
          continue;
        }
        const float rad = (angleDeg - 90.0f) * (3.14159265f / 180.0f);
        x2 = x + static_cast<int>(std::cos(rad) * length);
        y2 = y + static_cast<int>(std::sin(rad) * length);
      }
      const int thickness = std::max(1, node.thickness);
      const float dx = static_cast<float>(x2 - x);
      const float dy = static_cast<float>(y2 - y);
      const float len = std::sqrt(dx * dx + dy * dy);
      if (len < 0.0001f) {
        continue;
      }
      const float nx = -dy / len;
      const float ny = dx / len;
      for (int i = -(thickness / 2); i <= (thickness / 2); ++i) {
        const int ox = static_cast<int>(nx * i);
        const int oy = static_cast<int>(ny * i);
        drawLine(x + ox, y + oy, x2 + ox, y2 + oy, node.color565);
      }
      continue;
    }

    if (node.type == NodeType::kIcon) {
      const std::string rawPath = node.path.empty() ? node.text : node.path;
      const std::string iconPath = bindRuntimeTemplate(rawPath);
      if (iconPath.empty() || node.w <= 0 || node.h <= 0) {
        continue;
      }
      std::vector<uint16_t> pixels;
      const bool ok = isHttpUrl(iconPath) ? loadIconRemote(iconPath, node.w, node.h, pixels)
                                          : loadIcon(iconPath, node.w, node.h, pixels);
      if (!ok) {
        continue;
      }
      if (canvasActive()) {
        for (int iy = 0; iy < node.h; ++iy) {
          for (int ix = 0; ix < node.w; ++ix) {
            drawPixel(x + ix, y + iy,
                      pixels[static_cast<size_t>(iy) * static_cast<size_t>(node.w) + static_cast<size_t>(ix)]);
          }
        }
      } else {
        (void)display_spi::drawRgb565(static_cast<uint16_t>(x), static_cast<uint16_t>(y),
                                      static_cast<uint16_t>(node.w), static_cast<uint16_t>(node.h),
                                      pixels.data());
      }
      continue;
    }

    if (node.type == NodeType::kMoonPhase) {
      float phase = 0.0f;
      bool havePhase = false;
      if (!node.key.empty()) {
        havePhase = getNumeric(node.key, phase);
      }
      if (!havePhase) {
        havePhase = computeMoonPhaseFraction(phase);
      }
      if (!havePhase) {
        continue;
      }
      const int r = node.radius > 0 ? node.radius : (node.w > 0 ? node.w / 2 : 8);
      if (r <= 0) {
        continue;
      }
      drawCircle(x, y, r, node.bg565, true);
      const bool waxing = phase <= 0.5f;
      const float threshold = waxing ? (r * (1.0f - 2.0f * phase)) : (-r * (2.0f * phase - 1.0f));
      for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
          if (dx * dx + dy * dy > r * r) {
            continue;
          }
          const bool lit = waxing ? (dx > threshold) : (dx < threshold);
          if (lit) {
            drawPixel(x + dx, y + dy, node.color565);
          }
        }
      }
      if (node.thickness > 0) {
        drawCircle(x, y, r, node.color565, false);
      }
      continue;
    }
  }
}

const ModalSpec* findActiveModal() {
  if (s.activeModalId.empty()) {
    return nullptr;
  }
  for (const ModalSpec& modal : s.modals) {
    if (modal.id == s.activeModalId) {
      return &modal;
    }
  }
  return nullptr;
}

void drawWrappedTextBlock(int x, int y, int w, int h, const std::string& text, int scale, int lineHeight,
                          int maxLines, uint16_t fg, uint16_t bg) {
  if (w <= 0 || h <= 0) {
    return;
  }
  int lh = lineHeight > 0 ? lineHeight : (8 * scale);
  if (lh <= 0) {
    lh = 8;
  }
  int linesAllowed = maxLines > 0 ? maxLines : 0;
  if (h > 0) {
    const int byHeight = h / lh;
    if (byHeight > 0) {
      linesAllowed = linesAllowed > 0 ? std::min(linesAllowed, byHeight) : byHeight;
    }
  }
  std::vector<std::string> lines = wrapLabelLines(text, scale, w);
  bool truncated = false;
  if (linesAllowed > 0 && static_cast<int>(lines.size()) > linesAllowed) {
    lines.resize(static_cast<size_t>(linesAllowed));
    truncated = true;
  }
  if (truncated && !lines.empty()) {
    lines.back() = ellipsizeToWidth(lines.back(), scale, w);
  }
  for (size_t i = 0; i < lines.size(); ++i) {
    const int yy = y + static_cast<int>(i) * lh;
    if (yy + lh > y + h) {
      break;
    }
    drawText(x, yy, lines[i], fg, bg, scale);
  }
}

void renderActiveModal() {
  const ModalSpec* modal = findActiveModal();
  if (modal == nullptr) {
    return;
  }
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const size_t freeNow = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  if (largest < kUiCriticalLargest8Bit || freeNow < kUiCriticalFree8Bit) {
    ESP_LOGW(kTag, "modal dismissed low_heap widget=%s largest=%u free=%u", s.widgetId.c_str(),
             static_cast<unsigned>(largest), static_cast<unsigned>(freeNow));
    s.activeModalId.clear();
    s.modalDismissDueMs = 0;
    return;
  }
  const int mx = static_cast<int>(s.x) + modal->x;
  const int my = static_cast<int>(s.y) + modal->y;
  const int mw = modal->w;
  const int mh = modal->h;
  if (mw <= 0 || mh <= 0) {
    return;
  }

  drawSolidRect(mx, my, mw, mh, modal->bg565);
  drawSolidRect(mx, my, mw, 1, modal->border565);
  drawSolidRect(mx, my + mh - 1, mw, 1, modal->border565);
  drawSolidRect(mx, my, 1, mh, modal->border565);
  drawSolidRect(mx + mw - 1, my, 1, mh, modal->border565);

  const int scale = std::max(1, std::min(3, modal->font <= 1 ? 1 : (modal->font >= 4 ? 2 : 1)));
  const std::string title = bindRuntimeTemplate(modal->title);
  const std::string body = bindRuntimeTemplate(modal->text);
  drawWrappedTextBlock(mx + 6, my + 6, mw - 12, 28, title, scale, modal->lineHeight > 0 ? modal->lineHeight : 9,
                       2, modal->titleColor565, modal->bg565);
  drawSolidRect(mx + 4, my + 34, mw - 8, 1, modal->border565);
  drawWrappedTextBlock(mx + 6, my + 40, mw - 12, mh - 54, body, scale, modal->lineHeight, modal->maxLines,
                       modal->textColor565, modal->bg565);
}

void render() {
  if (!s.active) {
    return;
  }

  if (s.w == 0 || s.h == 0) {
    return;
  }

  const size_t bytesPerRow = static_cast<size_t>(s.w) * sizeof(uint16_t);
  if (bytesPerRow == 0) {
    return;
  }
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const size_t targetBytes = (largest * 3U) / 4U;
  uint16_t bandRows =
      static_cast<uint16_t>(std::clamp<size_t>(targetBytes / bytesPerRow, 1U, static_cast<size_t>(s.h)));
  size_t bandBytes = bytesPerRow * static_cast<size_t>(bandRows);
  sCanvas = static_cast<uint16_t*>(heap_caps_malloc(bandBytes, MALLOC_CAP_8BIT));
  while (sCanvas == nullptr && bandRows > 1) {
    bandRows = static_cast<uint16_t>(bandRows / 2U);
    bandBytes = bytesPerRow * static_cast<size_t>(bandRows);
    sCanvas = static_cast<uint16_t*>(heap_caps_malloc(bandBytes, MALLOC_CAP_8BIT));
  }

  if (sCanvas == nullptr) {
    ESP_LOGW(kTag, "widget=%s canvas alloc failed largest=%u row_bytes=%u; using direct draw",
             s.widgetId.c_str(), static_cast<unsigned>(largest), static_cast<unsigned>(bytesPerRow));
    drawSolidRect(s.x, s.y, s.w, s.h, kBg);
    drawSolidRect(s.x, s.y, s.w, 1, kBorder);
    drawSolidRect(s.x, static_cast<int>(s.y + s.h - 1), s.w, 1, kBorder);
    drawSolidRect(s.x, s.y, 1, s.h, kBorder);
    drawSolidRect(static_cast<int>(s.x + s.w - 1), s.y, 1, s.h, kBorder);
    drawText(static_cast<int>(s.x) + 4, static_cast<int>(s.y) + 4, s.widgetId, kAccent, kBg, 1);
    if (!s.hasData) {
      drawText(static_cast<int>(s.x) + 6, static_cast<int>(s.y) + 22, "LOADING...", kText, kBg, 1);
      return;
    }
    renderNodes();
    renderActiveModal();
    return;
  }

  sCanvasW = s.w;
  for (uint16_t row = 0; row < s.h; row = static_cast<uint16_t>(row + bandRows)) {
    const uint16_t rowsThis = std::min<uint16_t>(bandRows, static_cast<uint16_t>(s.h - row));
    sCanvasH = rowsThis;
    sCanvasY0 = static_cast<uint16_t>(s.y + row);
    std::fill(sCanvas, sCanvas + static_cast<size_t>(sCanvasW) * static_cast<size_t>(sCanvasH), kBg);

    drawSolidRect(s.x, s.y, s.w, 1, kBorder);
    drawSolidRect(s.x, static_cast<int>(s.y + s.h - 1), s.w, 1, kBorder);
    drawSolidRect(s.x, s.y, 1, s.h, kBorder);
    drawSolidRect(static_cast<int>(s.x + s.w - 1), s.y, 1, s.h, kBorder);
    drawText(static_cast<int>(s.x) + 4, static_cast<int>(s.y) + 4, s.widgetId, kAccent, kBg, 1);

    if (!s.hasData) {
      drawText(static_cast<int>(s.x) + 6, static_cast<int>(s.y) + 22, "LOADING...", kText, kBg, 1);
    } else {
      renderNodes();
      renderActiveModal();
    }

    (void)display_spi::drawRgb565(s.x, sCanvasY0, sCanvasW, sCanvasH, sCanvas);
  }

  heap_caps_free(sCanvas);
  sCanvas = nullptr;
  sCanvasW = 0;
  sCanvasH = 0;
  sCanvasY0 = 0;
}

}  // namespace

namespace dsl_widget_runtime {

void reset() {
  s = {};
  sInstances.clear();
}

bool begin(const char* widgetId, const char* dslPath, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
           const char* settingsJson, const char* sharedSettingsJson) {
  State previous = std::move(s);
  s = {};
  s.active = true;
  s.widgetId = (widgetId != nullptr) ? widgetId : "dsl";
  s.dslPath = (dslPath != nullptr) ? dslPath : "";
  s.x = x;
  s.y = y;
  s.w = w;
  s.h = h;
  loadWidgetSettings(settingsJson, sharedSettingsJson);
  loadTapActionFromSettings();

  const std::string dslJson = readFile(s.dslPath.c_str());
  if (dslJson.empty() || !loadDslConfig(dslJson)) {
    ESP_LOGE(kTag, "dsl load failed widget=%s path=%s", s.widgetId.c_str(), s.dslPath.c_str());
    s.active = false;
    s = std::move(previous);
    return false;
  }

  ESP_LOGI(kTag,
           "begin widget=%s path=%s source=%d poll_ms=%u fields=%u nodes=%u modals=%u touch_regions=%u settings=%u "
           "http_max=%u retain_source=%d",
           s.widgetId.c_str(), s.dslPath.c_str(), static_cast<int>(s.source),
           static_cast<unsigned>(s.pollMs), static_cast<unsigned>(s.fields.size()),
           static_cast<unsigned>(s.nodes.size()), static_cast<unsigned>(s.modals.size()),
           static_cast<unsigned>(s.touchRegions.size()), static_cast<unsigned>(s.settingValues.size()),
           static_cast<unsigned>(s.httpMaxBytes), s.retainSourceJson ? 1 : 0);

  render();
  sInstances.push_back(std::move(s));
  s = std::move(previous);
  return true;
}

bool tick(uint32_t nowMs) {
  bool drew = false;
  for (State& instance : sInstances) {
    s = std::move(instance);
    if (!s.active) {
      instance = std::move(s);
      continue;
    }

    if (s.backoffUntilMs != 0 && static_cast<int32_t>(nowMs - s.backoffUntilMs) < 0) {
      instance = std::move(s);
      continue;
    }

    if (!s.activeModalId.empty() && s.modalDismissDueMs != 0 &&
        static_cast<int32_t>(nowMs - s.modalDismissDueMs) >= 0) {
      ESP_LOGI(kTag, "modal auto close widget=%s modal=%s", s.widgetId.c_str(), s.activeModalId.c_str());
      s.activeModalId.clear();
      s.modalDismissDueMs = 0;
      render();
      drew = true;
    }

    const uint32_t cadence = s.hasData ? s.pollMs : kInitialPollMs;
    const bool cadenceDue = (s.lastFetchMs == 0 || (nowMs - s.lastFetchMs) >= cadence);
    const bool tapDue = (s.tapRefreshDueMs != 0 && static_cast<int32_t>(nowMs - s.tapRefreshDueMs) >= 0);
    if (cadenceDue || tapDue) {
      if (tapDue) {
        s.tapRefreshDueMs = 0;
      }
      const bool updated = fetchAndResolve(nowMs);
      if (updated) {
        s.hasData = true;
        s.lastFetchMs = nowMs;
        ESP_LOGI(kTag, "update ok widget=%s", s.widgetId.c_str());
      }
      render();
      drew = true;
    }
    instance = std::move(s);
  }
  s = {};
  return drew;
}

bool onTap(const char* widgetId, uint16_t localX, uint16_t localY) {
  if (widgetId == nullptr || *widgetId == '\0') {
    return false;
  }
  ESP_LOGI(kTag, "tap widget lookup widget=%s local=%u,%u", widgetId,
           static_cast<unsigned>(localX), static_cast<unsigned>(localY));
  for (State& instance : sInstances) {
    if (instance.widgetId != widgetId) {
      continue;
    }
    s = std::move(instance);
    ESP_LOGI(kTag, "tap widget found widget=%s action=%d", s.widgetId.c_str(),
             static_cast<int>(s.tapAction));

    if (!s.activeModalId.empty()) {
      ESP_LOGI(kTag, "tap modal close widget=%s modal=%s", s.widgetId.c_str(), s.activeModalId.c_str());
      s.activeModalId.clear();
      s.modalDismissDueMs = 0;
      render();
      instance = std::move(s);
      s = {};
      return true;
    }

    for (const TouchRegion& tr : s.touchRegions) {
      if (localX < static_cast<uint16_t>(std::max(0, tr.x)) ||
          localY < static_cast<uint16_t>(std::max(0, tr.y))) {
        continue;
      }
      const uint16_t tx2 = static_cast<uint16_t>(std::max(0, tr.x + tr.w));
      const uint16_t ty2 = static_cast<uint16_t>(std::max(0, tr.y + tr.h));
      if (localX >= tx2 || localY >= ty2) {
        continue;
      }
      if (tr.action == TouchActionType::kModal && !tr.modalId.empty()) {
        s.activeModalId = tr.modalId;
        s.modalDismissDueMs = tr.dismissMs > 0 ? (platform::millisMs() + tr.dismissMs) : 0;
        ESP_LOGI(kTag, "tap modal open widget=%s modal=%s", s.widgetId.c_str(), s.activeModalId.c_str());
        render();
        instance = std::move(s);
        s = {};
        return true;
      }
      if (tr.action == TouchActionType::kHttp && !tr.httpUrl.empty()) {
        std::string method = tr.httpMethod.empty() ? "POST" : tr.httpMethod;
        std::transform(method.begin(), method.end(), method.begin(), [](unsigned char c) {
          return static_cast<char>(std::toupper(c));
        });
        const std::string url = bindRuntimeTemplate(tr.httpUrl);
        const std::string body = bindRuntimeTemplate(tr.httpBody);
        std::vector<KeyValue> headers;
        headers.reserve(tr.httpHeaders.size() + 1);
        for (const auto& kv : tr.httpHeaders) {
          const std::string key = trimCopy(kv.first);
          const std::string value = bindRuntimeTemplate(kv.second);
          if (!key.empty() && !value.empty()) {
            headers.push_back({key, value});
          }
        }
        if (!body.empty()) {
          headers.push_back({"Content-Type",
                             tr.httpContentType.empty() ? "application/json" : tr.httpContentType});
        }
        int status = 0;
        std::string resp;
        std::string reason;
        const bool ok = httpRequest(method, url, headers, body, status, resp, reason);
        if (!ok || status < 200 || status >= 300) {
          ESP_LOGW(kTag, "tap touch_region http fail widget=%s status=%d reason=%s",
                   s.widgetId.c_str(), status, reason.c_str());
          instance = std::move(s);
          s = {};
          return false;
        }
        s.tapRefreshDueMs = platform::millisMs() + kTapPostHttpRefreshDelayMs;
        ESP_LOGI(kTag, "tap touch_region http ok widget=%s", s.widgetId.c_str());
        instance = std::move(s);
        s = {};
        return true;
      }
    }

    std::string reason;
    const bool ok = executeTapAction(reason);
    if (ok) {
      ESP_LOGI(kTag, "tap action ok widget=%s", s.widgetId.c_str());
      render();
    } else {
      ESP_LOGW(kTag, "tap action fail widget=%s reason=%s", s.widgetId.c_str(), reason.c_str());
    }
    instance = std::move(s);
    s = {};
    return ok;
  }
  return false;
}

bool isActive() { return !sInstances.empty(); }

}  // namespace dsl_widget_runtime
