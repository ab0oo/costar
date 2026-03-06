#include "DslWidgetRuntimeEspIdf.h"

#include "AppConfig.h"
#include "DisplaySpiEspIdf.h"
#include "DslJson.h"
#include "DslTime.h"
#include "Font5x7Classic.h"
#include "HttpTransportGate.h"
#include "RuntimeSettings.h"
#include "core/TimeSync.h"
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
#include "sdkconfig.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <limits>
#include <map>
#include <memory>
#include <sys/stat.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
#ifndef CONFIG_COSTAR_DSL_TLS_MIN_LARGEST_8BIT_KB
#ifdef COSTAR_DSL_TLS_MIN_LARGEST_8BIT_KB
#define CONFIG_COSTAR_DSL_TLS_MIN_LARGEST_8BIT_KB COSTAR_DSL_TLS_MIN_LARGEST_8BIT_KB
#else
#define CONFIG_COSTAR_DSL_TLS_MIN_LARGEST_8BIT_KB 16
#endif
#endif
#ifndef CONFIG_COSTAR_DSL_TLS_MIN_FREE_8BIT_KB
#ifdef COSTAR_DSL_TLS_MIN_FREE_8BIT_KB
#define CONFIG_COSTAR_DSL_TLS_MIN_FREE_8BIT_KB COSTAR_DSL_TLS_MIN_FREE_8BIT_KB
#else
// 30KB keeps headroom for TLS + small JSON payloads, while avoiding false
// guard defers in long-running fragmented states (~32KB free, 16KB largest).
#define CONFIG_COSTAR_DSL_TLS_MIN_FREE_8BIT_KB 30
#endif
#endif
#ifndef CONFIG_COSTAR_DSL_TLS_RELAXED_LARGEST_8BIT_KB
#ifdef COSTAR_DSL_TLS_RELAXED_LARGEST_8BIT_KB
#define CONFIG_COSTAR_DSL_TLS_RELAXED_LARGEST_8BIT_KB COSTAR_DSL_TLS_RELAXED_LARGEST_8BIT_KB
#else
#define CONFIG_COSTAR_DSL_TLS_RELAXED_LARGEST_8BIT_KB 18
#endif
#endif
#ifndef CONFIG_COSTAR_DSL_TLS_RELAXED_FREE_THRESHOLD_8BIT_KB
#ifdef COSTAR_DSL_TLS_RELAXED_FREE_THRESHOLD_8BIT_KB
#define CONFIG_COSTAR_DSL_TLS_RELAXED_FREE_THRESHOLD_8BIT_KB \
  COSTAR_DSL_TLS_RELAXED_FREE_THRESHOLD_8BIT_KB
#else
#define CONFIG_COSTAR_DSL_TLS_RELAXED_FREE_THRESHOLD_8BIT_KB 56
#endif
#endif

#if defined(CONFIG_MBEDTLS_CLIENT_SSL_SESSION_TICKETS)
constexpr int kMbedtlsClientSessionTicketsEnabled = 1;
#else
constexpr int kMbedtlsClientSessionTicketsEnabled = 0;
#endif

#if defined(CONFIG_MBEDTLS_SERVER_SSL_SESSION_TICKETS)
constexpr int kMbedtlsServerSessionTicketsEnabled = 1;
#else
constexpr int kMbedtlsServerSessionTicketsEnabled = 0;
#endif

#if defined(CONFIG_ESP_TLS_INSECURE)
constexpr int kEspTlsInsecureEnabled = 1;
#else
constexpr int kEspTlsInsecureEnabled = 0;
#endif

#if defined(CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY)
constexpr int kEspTlsSkipServerVerifyEnabled = 1;
#else
constexpr int kEspTlsSkipServerVerifyEnabled = 0;
#endif

#if defined(CONFIG_MBEDTLS_DYNAMIC_BUFFER)
constexpr int kMbedtlsDynamicBufferEnabled = 1;
#else
constexpr int kMbedtlsDynamicBufferEnabled = 0;
#endif

#if defined(CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)
constexpr int kMbedtlsKeepPeerCertificateEnabled = 1;
#else
constexpr int kMbedtlsKeepPeerCertificateEnabled = 0;
#endif

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
constexpr uint32_t kHttpWorkerReplyTimeoutMs = 20000U;
constexpr uint32_t kHttpWorkerStack = 8192U;
constexpr uint32_t kHttpResponseMaxBytesDefault = 16384U;
constexpr uint32_t kHttpResponseMaxBytesMin = 1024U;
constexpr uint32_t kHttpResponseMaxBytesMax = 32768U;
constexpr size_t kHttpCaptureSafetyMarginBytes = 256U;
constexpr UBaseType_t kHttpWorkerPriority = 4U;
constexpr BaseType_t kHttpWorkerCore = 0;
constexpr uint32_t kWsConnectTimeoutMs = 15000U;
constexpr uint32_t kWsDefaultKeepAliveMs = 30000U;
constexpr size_t kWsMaxFrameBytes = 8192U;
constexpr size_t kWsDiagLargeFrameBytes = 3000U;
constexpr uint32_t kTapPostHttpRefreshDelayMs = 750U;
constexpr size_t kIconMemCacheBudgetBytes = 24U * 1024U;
constexpr const char* kIconCacheDir = "/littlefs/icon_cache";
constexpr size_t kIconFileCacheBudgetBytes = 256U * 1024U;
constexpr size_t kIconFileCacheMaxEntries = 96U;
constexpr uint32_t kIconFetchRetryMs = 30000U;
constexpr size_t kIconRetryMaxEntries = 64U;
constexpr uint32_t kLowHeapRecoverCooldownMs = 1500U;
constexpr size_t kRuntimeValueSlotsMax = 192U;
constexpr size_t kValueInsertMinLargest8Bit = 4U * 1024U;
constexpr size_t kValueInsertMinFree8Bit = 12U * 1024U;
// Track pending WS requests briefly to tolerate async ack/event sequences.
constexpr uint32_t kWsPendingReqTimeoutMs = 8000U;
constexpr size_t kUiCriticalLargest8Bit = 12288U;
constexpr size_t kUiCriticalFree8Bit = 24576U;
// Guard against fragmentation before TLS setup. mbedtls_ssl_setup needs more
// than 16KB contiguous even with dynamic buffers; 20KB gives reliable headroom.
constexpr size_t kTlsMinLargest8Bit =
    static_cast<size_t>(CONFIG_COSTAR_DSL_TLS_MIN_LARGEST_8BIT_KB) * 1024U;
constexpr size_t kTlsMinFree8Bit =
    static_cast<size_t>(CONFIG_COSTAR_DSL_TLS_MIN_FREE_8BIT_KB) * 1024U;
// When total free heap is high, allow a slightly smaller contiguous block.
// This helps fragmented-but-healthy states proceed without disabling the guard.
constexpr size_t kTlsRelaxedLargest8Bit =
    static_cast<size_t>(CONFIG_COSTAR_DSL_TLS_RELAXED_LARGEST_8BIT_KB) * 1024U;
constexpr size_t kTlsRelaxedFreeThreshold8Bit =
    static_cast<size_t>(CONFIG_COSTAR_DSL_TLS_RELAXED_FREE_THRESHOLD_8BIT_KB) * 1024U;
// Once a WS connection is up, its TLS context is permanently resident. Only
// need enough heap for template string allocations (~3-5KB).
// Post-WS steady state is ~12-20KB largest / ~15-20KB free depending on the
// TLS handshake cost — use 8KB/12KB to allow fetching at this level.
constexpr size_t kWsActiveMinLargest8Bit = 8U * 1024U;
constexpr size_t kWsActiveMinFree8Bit = 12U * 1024U;
constexpr uint32_t kHttpReuseWindowMs = 500U;
constexpr size_t kHttpReuseMaxEntries = 8U;
constexpr size_t kCanvasPersistentMaxBytes = 16U * 1024U;
constexpr size_t kWsCacheJsonMaxEntries = 16U;
constexpr uint32_t kHttpStartupStaggerMs = 3000U;
constexpr uint32_t kHttpStartupStaggerMaxMs = 12000U;

enum class DataSource : uint8_t {
  kHttp,
  kWebSocket,
  kLocalTime,
  kUnknown,
};


struct FieldSpec {
  std::string key;
  std::string path;
  FormatSpec format;
};

size_t tlsMinLargestForFreeHeap(size_t freeHeap) {
  return (freeHeap >= kTlsRelaxedFreeThreshold8Bit) ? kTlsRelaxedLargest8Bit : kTlsMinLargest8Bit;
}

enum class NodeType : uint8_t {
  kLabel,
  kValueBox,
  kProgress,
  kSparkline,
  kIcon,
  kBitmap1,
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
  std::string xExpr;
  std::string yExpr;
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
  std::string visibleIf;
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
  kWsPublish,
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
  std::string wsUrl;
  std::string wsBody;
};

struct KeyValue {
  std::string key;
  std::string value;
};

struct HttpCapture {
  char* data = nullptr;
  size_t size = 0;
  size_t capacity = 0;
  size_t maxBytes = kHttpResponseMaxBytesDefault;
  bool overflow = false;
  bool oom = false;
};

struct HttpJob {
  std::string method;
  std::string url;
  std::string body;
  std::vector<KeyValue> headers;
  uint32_t maxResponseBytes = kHttpResponseMaxBytesDefault;
  bool tlsSkipVerify = false;
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

struct SharedHttpResponse {
  bool ok = false;
  int statusCode = 0;
  std::string body;
  std::string reason;
  uint32_t fetchedAtMs = 0;
};

enum class TapActionType : uint8_t {
  kNone,
  kRefresh,
  kHttp,
  kWsPublish,
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
  uint32_t lastDeferredLogMs = 0;
  uint8_t failureStreak = 0;
  bool tlsRetryGuard = false;
  TapActionType tapAction = TapActionType::kNone;
  std::string tapUrlTemplate;
  std::string tapMethod;
  std::string tapBodyTemplate;
  std::string tapContentType;
  std::vector<KeyValue> tapHeaders;
  std::string urlTemplate;
  std::string wsEntityTemplate;
  std::string wsCacheKeyTemplate;
  std::string wsSubscribeTemplate;
  std::string wsBootstrapTemplate;
  std::string wsResultPath;
  std::string wsEventPath;
  std::string wsAuthTemplate;
  std::string wsUrlTemplate;
  std::string wsTokenTemplate;
  std::string wsPathTemplate;
  std::string wsConnectionProfileTemplate;
  std::vector<KeyValue> headers;
  std::vector<std::string> transforms;
  std::vector<FieldSpec> fields;
  std::vector<Node> nodes;
  std::vector<ModalSpec> modals;
  std::vector<TouchRegion> touchRegions;
  std::string uiTitle;
  bool showTitle = false;
  std::vector<KeyValue> values;
  std::map<std::string, double> numericValues;
  std::map<std::string, std::string> transformValues;
  std::map<std::string, std::map<std::string, std::string>> localFormatLookups;
  std::map<std::string, std::string> settingValues;
  std::string activeModalId;
  uint32_t modalDismissDueMs = 0;
  bool drawBorder = true;
  std::string sourceJson;
  bool retainSourceJson = false;
  std::string transformJson;
  std::vector<std::pair<std::string, std::string>> transformScratch;
  uint32_t tapRefreshDueMs = 0;
  uint32_t httpMaxBytes = kHttpResponseMaxBytesDefault;
  bool tlsSkipVerify = false;
};

State s;
std::vector<State> sInstances;
QueueHandle_t sHttpJobQueue = nullptr;
TaskHandle_t sHttpWorkerTask = nullptr;
std::map<std::string, SharedHttpResponse> sSharedHttpCache;
std::string sSharedLookupsJsonCache;
std::map<std::string, std::map<std::string, std::string>> sSharedFormatLookups;
uint32_t sHttpStartupOrdinal = 0;
uint16_t* sCanvas = nullptr;
size_t sCanvasCapacityBytes = 0;
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
uint32_t sLastLowHeapRecoverMs = 0;

struct WsReqMeta {
  std::string cacheKey;
  std::string resultPath;
  std::string eventPath;
  bool isSubscribe = false;
  uint32_t sentAtMs = 0;
};

struct WsState {
  SemaphoreHandle_t lock = nullptr;
  esp_websocket_client_handle_t client = nullptr;
  std::string profileKey;
  std::string wsUrl;
  bool tlsSkipVerify = false;
  std::string token;
  std::string authTemplate;
  std::string authRequiredType = "auth_required";
  std::string authOkType = "auth_ok";
  std::string authInvalidType = "auth_invalid";
  std::string readyType = "ready";
  std::vector<std::string> initMessages;
  bool initSent = false;
  bool authOk = false;
  bool ready = false;
  bool started = false;
  bool bootstrapTriggerPending = false;
  uint32_t nextReqId = 1;
  uint32_t reconnectDueMs = 0;
  uint8_t failureStreak = 0;
  std::string rxFrame;
  std::map<std::string, std::string> cacheJsonByKey;
  std::map<uint32_t, WsReqMeta> pendingReqById;
  std::map<std::string, uint32_t> cacheKeyToPendingBootstrapReq;
  std::map<std::string, uint32_t> cacheKeyToPendingSubscribeReq;
  std::map<uint32_t, WsReqMeta> activeSubById;
  std::map<std::string, uint32_t> cacheKeyToActiveSubId;
};

WsState sWs;

bool reclaimRuntimeCachesLowHeap(const char* reason);

using namespace dsl_json;


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
  if (s.values.size() >= kRuntimeValueSlotsMax) {
    if (s.debug) {
      ESP_LOGW(kTag, "setValue skip key=%s reason=max_slots size=%u", key.c_str(),
               static_cast<unsigned>(s.values.size()));
    }
    return false;
  }
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const size_t freeNow = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  if (largest < kValueInsertMinLargest8Bit || freeNow < kValueInsertMinFree8Bit) {
    ESP_LOGW(kTag, "setValue skip key=%s reason=low_heap free=%u largest=%u", key.c_str(),
             static_cast<unsigned>(freeNow), static_cast<unsigned>(largest));
    return false;
  }
  s.values.push_back({key, value});
  return true;
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

  if (key == "geo.city") {
    return platform::prefs::getString("geo", "city", "");
  }
  if (key == "geo.region") {
    return platform::prefs::getString("geo", "region", "");
  }
  if (key == "geo.country") {
    return platform::prefs::getString("geo", "country", "");
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
    const std::string label = platform::prefs::getString("geo", "label", "");
    if (!label.empty()) {
      return label;
    }
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

std::string bindRuntimeTemplateWithMode(const std::string& input, bool preserveUnknownTokens) {
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
      if (preserveUnknownTokens) {
        start = out.find("{{", end + 2);
        continue;
      }
      value.clear();
    }

    out.replace(start, end - start + 2, value);
    start = out.find("{{");
  }

  return out;
}

std::string bindRuntimeTemplate(const std::string& input) {
  return bindRuntimeTemplateWithMode(input, false);
}

std::string bindRuntimeTemplatePreserveUnknown(const std::string& input) {
  return bindRuntimeTemplateWithMode(input, true);
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
bool ensureHttpWorker();
bool httpGet(const std::string& url, const std::vector<KeyValue>& headers, int& statusCode,
             std::string& body, std::string& reason);
bool httpGet(std::string&& url, std::vector<KeyValue>&& headers, int& statusCode, std::string& body,
             std::string& reason);
bool httpRequest(const std::string& method, const std::string& url,
                 const std::vector<KeyValue>& headers, const std::string& reqBody, int& statusCode,
                 std::string& body, std::string& reason);
bool httpRequest(std::string&& method, std::string&& url, std::vector<KeyValue>&& headers,
                 std::string&& reqBody, int& statusCode, std::string& body, std::string& reason);

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
  if (!objectMemberString(formatObj, "map_lookup", out.valueMapLookup)) {
    (void)objectMemberString(formatObj, "mapLookup", out.valueMapLookup);
  }
  std::string_view mapObj;
  if (objectMemberObject(formatObj, "map", mapObj)) {
    forEachObjectMember(mapObj, [&](const std::string& key, std::string_view valueText) {
      std::string value;
      if (viewToString(valueText, value)) {
        out.valueMap[key] = value;
      }
    });
  }
  if (!objectMemberString(formatObj, "map_default", out.valueMapDefault)) {
    (void)objectMemberString(formatObj, "default", out.valueMapDefault);
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
  if (type == "bitmap1" || type == "bitmap_1" || type == "mono_bitmap") return NodeType::kBitmap1;
  if (type == "moon_phase") return NodeType::kMoonPhase;
  if (type == "arc" || type == "circle") return NodeType::kArc;
  // "hand" is a deprecated alias retained for backward compatibility.
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

size_t estimateNodesCount(std::string_view nodesArray, const VarContext* vars, int depth = 0) {
  if (depth > 8) {
    return 0;
  }
  size_t total = 0;
  forEachArrayElement(nodesArray, [&](int /*idx*/, std::string_view nodeValue) {
    nodeValue = trimView(nodeValue);
    if (nodeValue.empty() || nodeValue.front() != '{') {
      return;
    }
    std::string type;
    if (!objectMemberString(nodeValue, "type", type)) {
      ++total;
      return;
    }
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    if (type != "repeat") {
      ++total;
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

    std::string_view childNodes;
    std::string_view singleNode;
    const bool hasNodes = objectMemberArray(nodeValue, "nodes", childNodes);
    const bool hasNode = objectMemberObject(nodeValue, "node", singleNode);
    size_t childCount = 0;
    if (hasNodes) {
      childCount = estimateNodesCount(childNodes, vars, depth + 1);
    } else if (hasNode) {
      childCount = 1;
    }
    total += childCount * static_cast<size_t>(count);
    if (total > 512) {
      total = 512;
    }
  });
  return total;
}

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

  node.xExpr = readStringValue(nodeObj, "x", vars, "");
  node.yExpr = readStringValue(nodeObj, "y", vars, "");
  (void)readIntValue(nodeObj, "x", vars, node.x);
  (void)readIntValue(nodeObj, "y", vars, node.y);
  (void)readIntValue(nodeObj, "w", vars, node.w);
  (void)readIntValue(nodeObj, "h", vars, node.h);
  (void)readIntValue(nodeObj, "x2", vars, node.x2);
  (void)readIntValue(nodeObj, "y2", vars, node.y2);
  (void)readIntValue(nodeObj, "r", vars, node.radius);
  (void)readIntValue(nodeObj, "length", vars, node.length);
  int thickness = node.thickness;
  const bool hasThickness = readIntValue(nodeObj, "thickness", vars, thickness);
  int width = node.thickness;
  const bool hasWidth = readIntValue(nodeObj, "width", vars, width);
  if (node.type == NodeType::kLine) {
    node.thickness = hasWidth ? width : thickness;
  } else if (hasThickness) {
    node.thickness = thickness;
  }
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
  node.visibleIf = readStringValue(nodeObj, "visible_if", vars, "");
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
  if (lower == "ws" || lower == "ws_publish" || lower == "websocket") {
    return TouchActionType::kWsPublish;
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
  } else if (out.action == TouchActionType::kWsPublish) {
    out.wsUrl = readStringValue(onTouchObj, "url", nullptr, "");
    out.wsBody = readStringValue(onTouchObj, "body", nullptr, "");
    if (trimCopy(out.wsUrl).empty() && trimCopy(out.wsBody).empty()) {
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

bool parseTransformNumericArg(std::string_view trObj, const char* key, double& out) {
  std::string_view valueText;
  if (!objectMemberValue(trObj, key, valueText)) {
    return false;
  }
  valueText = trimView(valueText);
  if (valueText.empty()) {
    return false;
  }
  if (valueText.front() != '"') {
    return viewToDouble(valueText, out);
  }
  std::string arg;
  if (!viewToString(valueText, arg)) {
    return false;
  }
  arg = bindRuntimeTemplate(arg);
  if (arg.empty()) {
    return false;
  }
  float exprValue = 0.0f;
  if (evalNumericExpr(arg, nullptr, exprValue)) {
    out = static_cast<double>(exprValue);
    return true;
  }
  return parseStrictDouble(arg, out);
}

void applyDistanceUnitScaleIfRequested(std::string_view trObj, double& value) {
  std::string unit = readStringValue(trObj, "unit", nullptr, "km");
  std::transform(unit.begin(), unit.end(), unit.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (unit == "mi" || unit == "mile" || unit == "miles") {
    value *= 1.609344;
  } else if (unit == "nm") {
    value *= 1.852;
  }
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

  double maxValue = 0.0;
  if (!parseTransformNumericArg(trObj, "max", maxValue)) {
    return false;
  }
  applyDistanceUnitScaleIfRequested(trObj, maxValue);

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

bool transformFilterGte(std::string_view trObj, std::map<std::string, std::vector<TransformRow>>& arrays) {
  const std::string from = readStringValue(trObj, "from", nullptr, "");
  const std::string by = readStringValue(trObj, "by", nullptr, "");
  if (from.empty() || by.empty()) {
    return false;
  }
  auto it = arrays.find(from);
  if (it == arrays.end()) {
    return false;
  }

  double minValue = 0.0;
  if (!parseTransformNumericArg(trObj, "min", minValue)) {
    return false;
  }
  applyDistanceUnitScaleIfRequested(trObj, minValue);

  std::vector<TransformRow> filtered;
  filtered.reserve(it->second.size());
  for (const TransformRow& row : it->second) {
    double value = 0.0;
    if (!rowNumeric(row, by, value)) {
      continue;
    }
    if (value >= minValue) {
      filtered.push_back(row);
    }
  }
  it->second = std::move(filtered);
  return true;
}

bool transformFilterBetween(std::string_view trObj, std::map<std::string, std::vector<TransformRow>>& arrays) {
  const std::string from = readStringValue(trObj, "from", nullptr, "");
  const std::string by = readStringValue(trObj, "by", nullptr, "");
  if (from.empty() || by.empty()) {
    return false;
  }
  auto it = arrays.find(from);
  if (it == arrays.end()) {
    return false;
  }

  double minValue = 0.0;
  double maxValue = 0.0;
  if (!parseTransformNumericArg(trObj, "min", minValue) ||
      !parseTransformNumericArg(trObj, "max", maxValue)) {
    return false;
  }
  applyDistanceUnitScaleIfRequested(trObj, minValue);
  applyDistanceUnitScaleIfRequested(trObj, maxValue);
  if (minValue > maxValue) {
    std::swap(minValue, maxValue);
  }

  bool inclusive = true;
  (void)objectMemberBool(trObj, "inclusive", inclusive);

  std::vector<TransformRow> filtered;
  filtered.reserve(it->second.size());
  for (const TransformRow& row : it->second) {
    double value = 0.0;
    if (!rowNumeric(row, by, value)) {
      continue;
    }
    const bool inRange = inclusive ? (value >= minValue && value <= maxValue)
                                   : (value > minValue && value < maxValue);
    if (inRange) {
      filtered.push_back(row);
    }
  }
  it->second = std::move(filtered);
  return true;
}

bool transformProjectLatLon(std::string_view trObj, std::map<std::string, std::vector<TransformRow>>& arrays) {
  const std::string from = readStringValue(trObj, "from", nullptr, "");
  if (from.empty()) {
    return false;
  }
  auto it = arrays.find(from);
  if (it == arrays.end()) {
    return false;
  }

  const std::string latPath = readStringValue(trObj, "lat_path", nullptr, "lat");
  const std::string lonPath = readStringValue(trObj, "lon_path", nullptr, "lon");
  const std::string xField = readStringValue(
      trObj, "x_field", nullptr, readStringValue(trObj, "to_x_field", nullptr, "x"));
  const std::string yField = readStringValue(
      trObj, "y_field", nullptr, readStringValue(trObj, "to_y_field", nullptr, "y"));
  if (latPath.empty() || lonPath.empty() || xField.empty() || yField.empty()) {
    return false;
  }

  double lonMin = 0.0;
  double lonMax = 0.0;
  double latMin = 0.0;
  double latMax = 0.0;
  double xMin = 0.0;
  double xMax = 319.0;
  double yMin = 0.0;
  double yMax = 239.0;
  if (!parseTransformNumericArg(trObj, "lon_min", lonMin) ||
      !parseTransformNumericArg(trObj, "lon_max", lonMax) ||
      !parseTransformNumericArg(trObj, "lat_min", latMin) ||
      !parseTransformNumericArg(trObj, "lat_max", latMax)) {
    return false;
  }
  (void)parseTransformNumericArg(trObj, "x_min", xMin);
  (void)parseTransformNumericArg(trObj, "x_max", xMax);
  (void)parseTransformNumericArg(trObj, "y_min", yMin);
  (void)parseTransformNumericArg(trObj, "y_max", yMax);

  if (std::fabs(lonMax - lonMin) < 0.000001 || std::fabs(latMax - latMin) < 0.000001) {
    return false;
  }

  bool clamp = true;
  (void)objectMemberBool(trObj, "clamp", clamp);

  for (TransformRow& row : it->second) {
    double lat = 0.0;
    double lon = 0.0;
    if (!rowNumeric(row, latPath, lat) || !rowNumeric(row, lonPath, lon)) {
      continue;
    }

    double nx = (lon - lonMin) / (lonMax - lonMin);
    double ny = (latMax - lat) / (latMax - latMin);
    if (clamp) {
      nx = std::clamp(nx, 0.0, 1.0);
      ny = std::clamp(ny, 0.0, 1.0);
    }
    const double x = xMin + nx * (xMax - xMin);
    const double y = yMin + ny * (yMax - yMin);

    char xb[32];
    char yb[32];
    std::snprintf(xb, sizeof(xb), "%.3f", x);
    std::snprintf(yb, sizeof(yb), "%.3f", y);
    row.fields[xField] = xb;
    row.fields[yField] = yb;
  }
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
                        std::vector<std::pair<std::string, std::string>>& outFlat) {
  constexpr size_t kNpos = static_cast<size_t>(-1);
  const auto findFlat = [&](std::string_view key) {
    for (size_t i = 0; i < outFlat.size(); ++i) {
      if (outFlat[i].first == key) {
        return i;
      }
    }
    return kNpos;
  };
  const auto upsertFlat = [&](const std::string& key, std::string value) {
    const size_t idx = findFlat(key);
    if (idx == kNpos) {
      outFlat.emplace_back(key, std::move(value));
    } else {
      outFlat[idx].second = std::move(value);
    }
  };
  const auto uniqueFlatCount = [&]() {
    size_t unique = 0;
    for (size_t i = 0; i < outFlat.size(); ++i) {
      bool duplicate = false;
      for (size_t j = 0; j < i; ++j) {
        if (outFlat[j].first == outFlat[i].first) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        ++unique;
      }
    }
    return unique;
  };

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

  // Guard against generating more transform keys than runtime value slots allow.
  // This prevents setValue(max_slots) churn when index_rows count/fields are oversized.
  std::vector<std::string> prefixes;
  prefixes.reserve(fields.size());
  for (const std::string& field : fields) {
    const std::string prefix = prefixMap.count(field) != 0 ? prefixMap[field] : field;
    if (std::find(prefixes.begin(), prefixes.end(), prefix) == prefixes.end()) {
      prefixes.push_back(prefix);
    }
  }
  const size_t perRowKeys = prefixes.empty() ? 1U : prefixes.size();
  const size_t currentKeys = uniqueFlatCount();
  const size_t countKeyCost = (countKey.empty() || findFlat(countKey) != kNpos) ? 0U : 1U;
  const size_t availableAfterCountKey =
      (kRuntimeValueSlotsMax > currentKeys + countKeyCost) ? (kRuntimeValueSlotsMax - currentKeys - countKeyCost) : 0U;
  const size_t maxRowsBySlots = availableAfterCountKey / perRowKeys;
  const int requestedCount = count;
  if (static_cast<size_t>(count) > maxRowsBySlots) {
    count = static_cast<int>(maxRowsBySlots);
    ESP_LOGW(kTag,
             "index_rows cap from=%s requested=%d capped=%d fields=%u per_row=%u flat_keys=%u max_slots=%u",
             from.c_str(), requestedCount, count, static_cast<unsigned>(fields.size()),
             static_cast<unsigned>(perRowKeys), static_cast<unsigned>(currentKeys),
             static_cast<unsigned>(kRuntimeValueSlotsMax));
  }

  upsertFlat(countKey,
             std::to_string(static_cast<int>(std::min<size_t>(it->second.size(), static_cast<size_t>(count)))));
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
      upsertFlat(key, std::move(value));
    }
  }
  return true;
}

void applyTransforms(std::string_view rootJson) {
  std::map<std::string, std::vector<TransformRow>> arrays;
  s.transformScratch.clear();
  if (s.transformScratch.capacity() < kRuntimeValueSlotsMax) {
    s.transformScratch.reserve(kRuntimeValueSlotsMax);
  }

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
    } else if (op == "filter_gte") {
      ok = transformFilterGte(trObj, arrays);
    } else if (op == "filter_between") {
      ok = transformFilterBetween(trObj, arrays);
    } else if (op == "project_latlon") {
      ok = transformProjectLatLon(trObj, arrays);
    } else if (op == "sort") {
      ok = transformSort(trObj, arrays);
    } else if (op == "take") {
      ok = transformTake(trObj, arrays);
    } else if (op == "index_rows") {
      ok = transformIndexRows(trObj, arrays, s.transformScratch);
    }
    if (!ok && s.debug) {
      ESP_LOGW(kTag, "transform op failed: %s", op.c_str());
    }
  }

  const auto hasFlatKey = [&](const std::string& key) {
    for (const auto& kv : s.transformScratch) {
      if (kv.first == key) {
        return true;
      }
    }
    return false;
  };

  for (auto& prev : s.transformValues) {
    if (!hasFlatKey(prev.first) && !prev.second.empty()) {
      (void)setValue(prev.first, "");
      s.numericValues.erase(prev.first);
      prev.second.clear();
    }
  }
  for (const auto& kv : s.transformScratch) {
    (void)setValue(kv.first, kv.second);
    double numeric = 0.0;
    if (parseStrictDouble(kv.second, numeric)) {
      s.numericValues[kv.first] = numeric;
    } else {
      s.numericValues.erase(kv.first);
    }
    auto it = s.transformValues.find(kv.first);
    if (it == s.transformValues.end()) {
      s.transformValues.emplace(kv.first, kv.second);
    } else {
      it->second = kv.second;
    }
  }
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
  } else if (source == "websocket" || source == "ws") {
    s.source = DataSource::kWebSocket;
  } else if (source == "local_time") {
    s.source = DataSource::kLocalTime;
  } else {
    s.source = DataSource::kUnknown;
  }

  s.urlTemplate.clear();
  s.wsEntityTemplate.clear();
  s.wsCacheKeyTemplate.clear();
  s.wsSubscribeTemplate.clear();
  s.wsBootstrapTemplate.clear();
  s.wsResultPath.clear();
  s.wsEventPath.clear();
  s.wsAuthTemplate.clear();
  s.wsUrlTemplate.clear();
  s.wsTokenTemplate.clear();
  s.wsPathTemplate.clear();
  s.wsConnectionProfileTemplate.clear();
  s.headers.clear();
  s.transforms.clear();
  s.localFormatLookups.clear();
  (void)objectMemberString(dataObj, "url", s.urlTemplate);
  (void)objectMemberString(dataObj, "entity_id", s.wsEntityTemplate);
  (void)objectMemberString(dataObj, "cache_key", s.wsCacheKeyTemplate);
  (void)objectMemberString(dataObj, "result_path", s.wsResultPath);
  (void)objectMemberString(dataObj, "event_path", s.wsEventPath);
  (void)objectMemberString(dataObj, "auth_message", s.wsAuthTemplate);
  (void)objectMemberString(dataObj, "ws_url", s.wsUrlTemplate);
  (void)objectMemberString(dataObj, "ws_token", s.wsTokenTemplate);
  (void)objectMemberString(dataObj, "ws_path", s.wsPathTemplate);
  (void)objectMemberString(dataObj, "connection_profile", s.wsConnectionProfileTemplate);
  std::string_view wsSubscribeValue;
  if (objectMemberValue(dataObj, "subscribe", wsSubscribeValue)) {
    wsSubscribeValue = trimView(wsSubscribeValue);
    if (!wsSubscribeValue.empty()) {
      s.wsSubscribeTemplate.assign(wsSubscribeValue.data(), wsSubscribeValue.size());
    }
  }
  std::string_view wsBootstrapValue;
  if (objectMemberValue(dataObj, "bootstrap", wsBootstrapValue)) {
    wsBootstrapValue = trimView(wsBootstrapValue);
    if (!wsBootstrapValue.empty()) {
      s.wsBootstrapTemplate.assign(wsBootstrapValue.data(), wsBootstrapValue.size());
    }
  }
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
  std::string_view lookupsObj;
  if (objectMemberObject(dataObj, "lookups", lookupsObj)) {
    forEachObjectMember(lookupsObj, [&](const std::string& lookupName, std::string_view lookupValue) {
      lookupValue = trimView(lookupValue);
      if (lookupValue.empty() || lookupValue.front() != '{') {
        return;
      }
      std::map<std::string, std::string> lookupMap;
      forEachObjectMember(lookupValue, [&](const std::string& mapKey, std::string_view mapValue) {
        lookupMap[mapKey] = valueViewToText(mapValue);
      });
      if (!lookupMap.empty()) {
        s.localFormatLookups[lookupName] = std::move(lookupMap);
      }
    });
  }

  int poll = static_cast<int>(kDefaultPollMs);
  if (objectMemberInt(dataObj, "poll_ms", poll) && poll > 0) {
    s.pollMs = static_cast<uint32_t>(poll);
  } else {
    s.pollMs = kDefaultPollMs;
  }

  int httpMaxFromData = 0;
  bool tlsSkipVerifyFromData = false;
  bool hasTlsSkipVerifyFromData = false;
  if (objectMemberInt(dataObj, "http_max_bytes", httpMaxFromData) ||
      objectMemberInt(dataObj, "max_response_bytes", httpMaxFromData)) {
    if (httpMaxFromData > 0) {
      s.httpMaxBytes =
          std::clamp<uint32_t>(static_cast<uint32_t>(httpMaxFromData),
                               kHttpResponseMaxBytesMin, kHttpResponseMaxBytesMax);
    }
  }
  if (objectMemberBool(dataObj, "tls_skip_verify", tlsSkipVerifyFromData) ||
      objectMemberBool(dataObj, "tls_insecure", tlsSkipVerifyFromData)) {
    hasTlsSkipVerifyFromData = true;
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
  s.tlsSkipVerify = false;
  s.retainSourceJson = false;
  if (hasTlsSkipVerifyFromData) {
    s.tlsSkipVerify = tlsSkipVerifyFromData;
  }

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

  s.values.reserve(std::min<size_t>(kRuntimeValueSlotsMax, s.fields.size() + 24U));
  for (const FieldSpec& f : s.fields) {
    s.values.push_back({f.key, ""});
  }

  std::string_view uiObj;
  if (objectMemberObject(root, "ui", uiObj)) {
    s.uiTitle.clear();
    s.showTitle = false;
    (void)objectMemberString(uiObj, "title", s.uiTitle);
    (void)objectMemberBool(uiObj, "show_title", s.showTitle);

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
      const size_t estimatedNodes = estimateNodesCount(nodesArray, nullptr);
      if (estimatedNodes > 0) {
        const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        const size_t reserveBytes = estimatedNodes * sizeof(Node);
        if (largest < reserveBytes + 4096U) {
          ESP_LOGW(kTag, "dsl nodes reserve skipped widget=%s est_nodes=%u need=%u largest=%u",
                   s.widgetId.c_str(), static_cast<unsigned>(estimatedNodes),
                   static_cast<unsigned>(reserveBytes), static_cast<unsigned>(largest));
          return false;
        }
        s.nodes.reserve(estimatedNodes);
      }
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
  if (s.source == DataSource::kWebSocket) {
    if (s.wsEntityTemplate.empty()) {
      s.wsEntityTemplate = "{{setting.entity_id}}";
    }
    if (s.wsCacheKeyTemplate.empty()) {
      if (!s.wsEntityTemplate.empty()) {
        s.wsCacheKeyTemplate = s.wsEntityTemplate;
      } else {
        s.wsCacheKeyTemplate = s.widgetId;
      }
    }
    if (s.wsResultPath.empty()) {
      s.wsResultPath = "result";
    }
    if (s.wsEventPath.empty()) {
      s.wsEventPath = "event";
    }
  }
  if (auto it = s.settingValues.find("http_max_bytes"); it != s.settingValues.end()) {
    double parsed = 0.0;
    if (parseStrictDouble(it->second, parsed) && std::isfinite(parsed)) {
      const uint32_t v = static_cast<uint32_t>(std::lround(parsed));
      s.httpMaxBytes = std::clamp<uint32_t>(v, kHttpResponseMaxBytesMin, kHttpResponseMaxBytesMax);
    }
  }
  if (auto it = s.settingValues.find("tls_skip_verify"); it != s.settingValues.end()) {
    const std::string v = trimCopy(it->second);
    std::string low = v;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    if (low == "1" || low == "true" || low == "yes" || low == "on") {
      s.tlsSkipVerify = true;
    } else if (low == "0" || low == "false" || low == "no" || low == "off") {
      s.tlsSkipVerify = false;
    }
  }

  for (const Node& n : s.nodes) {
    // Icon/bitmap node paths are filesystem/URL paths, never looked up in sourceJson.
    if (n.type != NodeType::kIcon && n.type != NodeType::kBitmap1 && !n.path.empty()) {
      s.retainSourceJson = true;
      break;
    }
  }
  // Websocket sources keep payloads in shared cache; avoid per-widget duplicate
  // source JSON unless explicitly re-enabled.
  if (s.source == DataSource::kWebSocket) {
    s.retainSourceJson = false;
  }
  if (auto it = s.settingValues.find("retain_source_json"); it != s.settingValues.end()) {
    const std::string v = trimCopy(it->second);
    const std::string low = [&]() {
      std::string out = v;
      std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return out;
    }();
    if (low == "1" || low == "true" || low == "yes" || low == "on") {
      s.retainSourceJson = true;
    } else if (low == "0" || low == "false" || low == "no" || low == "off") {
      s.retainSourceJson = false;
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

bool ensureHttpCaptureCapacity(HttpCapture* cap, size_t needBytes) {
  if (cap == nullptr) {
    return false;
  }
  if (needBytes > cap->maxBytes) {
    cap->overflow = true;
    return false;
  }
  if (needBytes <= cap->capacity && cap->data != nullptr) {
    return true;
  }

  size_t target = needBytes;
  if (target < cap->maxBytes) {
    target = std::min(cap->maxBytes, target + kHttpCaptureSafetyMarginBytes);
  }
  if (target <= cap->capacity && cap->data != nullptr) {
    return true;
  }

  void* grown = nullptr;
  if (cap->data == nullptr) {
    grown = heap_caps_malloc(target + 1U, MALLOC_CAP_8BIT);
    if (grown != nullptr && cap->size == 0) {
      static_cast<char*>(grown)[0] = '\0';
    }
  } else {
    grown = heap_caps_realloc(cap->data, target + 1U, MALLOC_CAP_8BIT);
  }
  if (grown == nullptr) {
    cap->oom = true;
    const size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    ESP_LOGW(kTag,
             "http capture alloc fail need=%u target=%u cap=%u free=%u largest=%u",
             static_cast<unsigned>(needBytes), static_cast<unsigned>(target),
             static_cast<unsigned>(cap->maxBytes), static_cast<unsigned>(freeHeap),
             static_cast<unsigned>(largest));
    return false;
  }
  cap->data = static_cast<char*>(grown);
  cap->capacity = target;
  if (cap->size <= cap->capacity) {
    cap->data[cap->size] = '\0';
  }
  return true;
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
          (void)ensureHttpCaptureCapacity(cap, contentLen);
        }
      }
    }
  }
  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data != nullptr && evt->data_len > 0) {
    if (cap->overflow || cap->oom) {
      return ESP_OK;
    }
    const size_t needBytes = cap->size + static_cast<size_t>(evt->data_len);
    if (!ensureHttpCaptureCapacity(cap, needBytes)) {
      return ESP_OK;
    }
    std::memcpy(cap->data + cap->size, evt->data, static_cast<size_t>(evt->data_len));
    cap->size += static_cast<size_t>(evt->data_len);
    cap->data[cap->size] = '\0';
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

bool urlUsesTls(const std::string& url) {
  return url.rfind("https://", 0) == 0 || url.rfind("wss://", 0) == 0;
}

bool tlsClockReady() {
  return timesync::isUtcTimeReady();
}

std::string buildHttpReuseKey(const std::string& url, const std::vector<KeyValue>& headers,
                              uint32_t maxResponseBytes) {
  std::string key = url;
  key += "|max=";
  key += std::to_string(static_cast<unsigned>(maxResponseBytes));
  for (const KeyValue& kv : headers) {
    key += "|";
    key += kv.key;
    key += "=";
    key += kv.value;
  }
  return key;
}

void pruneHttpReuseCache(uint32_t nowMs) {
  for (auto it = sSharedHttpCache.begin(); it != sSharedHttpCache.end();) {
    const uint32_t ageMs = nowMs - it->second.fetchedAtMs;
    if (ageMs > kHttpReuseWindowMs) {
      it = sSharedHttpCache.erase(it);
      continue;
    }
    ++it;
  }
  while (sSharedHttpCache.size() > kHttpReuseMaxEntries) {
    auto oldest = sSharedHttpCache.end();
    for (auto it = sSharedHttpCache.begin(); it != sSharedHttpCache.end(); ++it) {
      if (oldest == sSharedHttpCache.end() || it->second.fetchedAtMs < oldest->second.fetchedAtMs) {
        oldest = it;
      }
    }
    if (oldest == sSharedHttpCache.end()) {
      break;
    }
    sSharedHttpCache.erase(oldest);
  }
}

const SharedHttpResponse* readHttpReuseCache(const std::string& key, uint32_t nowMs) {
  pruneHttpReuseCache(nowMs);
  auto it = sSharedHttpCache.find(key);
  if (it == sSharedHttpCache.end()) {
    return nullptr;
  }
  return &it->second;
}

void writeHttpReuseCache(const std::string& key, SharedHttpResponse&& value) {
  pruneHttpReuseCache(platform::millisMs());
  sSharedHttpCache[key] = std::move(value);
}

void logTlsFailureEpoch(const std::string& url, esp_err_t err) {
  if (!urlUsesTls(url)) {
    return;
  }
  const time_t nowUtc = std::time(nullptr);
  const uint32_t heapFree = static_cast<uint32_t>(esp_get_free_heap_size());
  const uint32_t heapLargest =
      static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  ESP_LOGW(kTag,
           "tls diag epoch=%lld clock_ready=%d err=%s heap_free=%u heap_largest=%u url=%s",
           static_cast<long long>(nowUtc), tlsClockReady() ? 1 : 0, esp_err_to_name(err),
           static_cast<unsigned>(heapFree), static_cast<unsigned>(heapLargest), url.c_str());
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

void parseLookupsJsonIntoMap(const char* lookupsJson,
                             std::map<std::string, std::map<std::string, std::string>>& outMap) {
  outMap.clear();
  if (lookupsJson == nullptr || *lookupsJson == '\0') {
    return;
  }
  const std::string_view root = trimView(lookupsJson);
  if (root.empty() || root.front() != '{') {
    return;
  }
  forEachObjectMember(root, [&](const std::string& lookupName, std::string_view lookupValue) {
    lookupValue = trimView(lookupValue);
    if (lookupValue.empty() || lookupValue.front() != '{') {
      return;
    }
    std::map<std::string, std::string> lookupMap;
    forEachObjectMember(lookupValue, [&](const std::string& mapKey, std::string_view mapValue) {
      lookupMap[mapKey] = valueViewToText(mapValue);
    });
    if (!lookupMap.empty()) {
      outMap[lookupName] = std::move(lookupMap);
    }
  });
}

void refreshSharedLookupsCache(const char* sharedLookupsJson) {
  const char* raw = (sharedLookupsJson != nullptr) ? sharedLookupsJson : "";
  if (sSharedLookupsJsonCache == raw) {
    return;
  }
  sSharedLookupsJsonCache = raw;
  parseLookupsJsonIntoMap(raw, sSharedFormatLookups);
}

const std::map<std::string, std::string>* findLookupMapByName(const std::string& lookupName) {
  if (lookupName.empty()) {
    return nullptr;
  }
  auto localIt = s.localFormatLookups.find(lookupName);
  if (localIt != s.localFormatLookups.end()) {
    return &localIt->second;
  }
  auto sharedIt = sSharedFormatLookups.find(lookupName);
  if (sharedIt != sSharedFormatLookups.end()) {
    return &sharedIt->second;
  }
  return nullptr;
}

bool resolveMappedLookupValue(const FormatSpec& fmt, const std::string& rawText, bool numeric, double numericValue,
                              std::string& outValue) {
  if (fmt.valueMapLookup.empty()) {
    return false;
  }
  const std::map<std::string, std::string>* lookup = findLookupMapByName(fmt.valueMapLookup);
  if (lookup == nullptr) {
    if (fmt.valueMapDefault.empty()) {
      return false;
    }
    outValue = fmt.valueMapDefault;
    return true;
  }

  std::string key = trimCopy(rawText);
  auto it = lookup->find(key);
  if (it == lookup->end() && numeric && std::isfinite(numericValue)) {
    const double rounded = std::round(numericValue);
    if (std::fabs(numericValue - rounded) < 0.000001) {
      key = std::to_string(static_cast<long long>(rounded));
      it = lookup->find(key);
    }
  }
  if (it != lookup->end()) {
    outValue = it->second;
    return true;
  }
  if (!fmt.valueMapDefault.empty()) {
    outValue = fmt.valueMapDefault;
    return true;
  }
  return false;
}

std::string readSetting(const std::string& key, const std::string& fallback) {
  auto it = s.settingValues.find(key);
  if (it == s.settingValues.end()) {
    return fallback;
  }
  return it->second;
}

const std::string* readSettingRef(const std::string& key) {
  auto it = s.settingValues.find(key);
  if (it == s.settingValues.end()) {
    return nullptr;
  }
  return &it->second;
}

struct WsProfileConfig {
  std::string url;
  std::string path;
  std::string token;
  std::string authMessage;
  std::string authRequiredType = "auth_required";
  std::string authOkType = "auth_ok";
  std::string authInvalidType = "auth_invalid";
  std::string readyType = "ready";
  std::vector<std::string> initMessages;
};

bool takeWsLock(uint32_t timeoutMs) {
  if (sWs.lock == nullptr) {
    sWs.lock = xSemaphoreCreateMutex();
    if (sWs.lock == nullptr) {
      return false;
    }
  }
  return xSemaphoreTake(sWs.lock, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void giveWsLock() {
  if (sWs.lock != nullptr) {
    xSemaphoreGive(sWs.lock);
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

uint32_t nextWsReqIdLocked() {
  if (sWs.nextReqId == 0) {
    sWs.nextReqId = 1;
  }
  return sWs.nextReqId++;
}

bool wsSendTextLocked(const std::string& payload) {
  if (sWs.client == nullptr || payload.empty()) {
    return false;
  }
  const int sent = esp_websocket_client_send_text(sWs.client, payload.c_str(),
                                                  static_cast<int>(payload.size()),
                                                  pdMS_TO_TICKS(1000));
  return sent >= 0;
}

bool buildWsMessageWithId(std::string message, uint32_t id, std::string& out) {
  out.clear();
  std::string_view trimmed = trimView(message);
  if (trimmed.empty() || trimmed.front() != '{' || trimmed.back() != '}') {
    return false;
  }

  std::string_view idView;
  if (objectMemberValue(trimmed, "id", idView)) {
    out.assign(trimmed.data(), trimmed.size());
    return true;
  }

  if (trimmed.size() == 2) {
    out = "{\"id\":";
    out += std::to_string(static_cast<unsigned>(id));
    out += "}";
    return true;
  }

  out = "{\"id\":";
  out += std::to_string(static_cast<unsigned>(id));
  out += ",";
  out.append(trimmed.data() + 1, trimmed.size() - 2);
  out.push_back('}');
  return true;
}

void wsStoreCacheLocked(const std::string& cacheKey, const std::string& jsonPayload, const char* kind,
                        const std::string& path) {
  if (cacheKey.empty() || jsonPayload.empty()) {
    return;
  }
  sWs.cacheJsonByKey[cacheKey] = jsonPayload;
  while (sWs.cacheJsonByKey.size() > kWsCacheJsonMaxEntries) {
    sWs.cacheJsonByKey.erase(sWs.cacheJsonByKey.begin());
  }
  sWs.bootstrapTriggerPending = true;
  const size_t previewLen = std::min<size_t>(jsonPayload.size(), 160U);
  ESP_LOGI(kTag, "ws cache update key=%s kind=%s path=%s bytes=%u preview=%.*s", cacheKey.c_str(),
           kind != nullptr ? kind : "unknown", path.c_str(),
           static_cast<unsigned>(jsonPayload.size()), static_cast<int>(previewLen),
           jsonPayload.c_str());
}

bool wsExtractPayloadByPath(std::string_view message, const std::string& path, std::string& out) {
  out.clear();
  std::string_view valueView;
  if (path.empty()) {
    return false;
  }
  if (!resolveJsonPath(message, path, valueView)) {
    return false;
  }
  out = valueViewToText(valueView);
  return !out.empty();
}

void wsSendAuthLocked() {
  if (sWs.client == nullptr || sWs.token.empty()) {
    return;
  }

  std::string authPayload = trimCopy(sWs.authTemplate);
  if (authPayload.empty()) {
    authPayload =
        std::string("{\"type\":\"auth\",\"access_token\":\"") + jsonEscape(sWs.token) + "\"}";
  } else {
    authPayload = replaceAll(authPayload, "{{ws_token}}", jsonEscape(sWs.token));
    authPayload = replaceAll(authPayload, "{{token}}", jsonEscape(sWs.token));
    authPayload = replaceAll(authPayload, "{{setting.ws_token}}", jsonEscape(sWs.token));
  }

  (void)wsSendTextLocked(authPayload);
}

void wsSendInitLocked() {
  if (sWs.client == nullptr || sWs.initSent) {
    return;
  }
  for (const std::string& rawMsg : sWs.initMessages) {
    std::string msg = trimCopy(rawMsg);
    if (msg.empty()) {
      continue;
    }
    msg = replaceAll(msg, "{{ws_token}}", jsonEscape(sWs.token));
    msg = replaceAll(msg, "{{token}}", jsonEscape(sWs.token));
    msg = replaceAll(msg, "{{setting.ws_token}}", jsonEscape(sWs.token));
    (void)wsSendTextLocked(msg);
  }
  sWs.initSent = true;
}

void wsHandleMessageLocked(std::string_view message) {
  std::string type;
  if (!objectMemberString(message, "type", type) || type.empty()) {
    ESP_LOGW(kTag, "ws msg no-type bytes=%u", static_cast<unsigned>(message.size()));
    return;
  }
  // Log all incoming message types for diagnostics (auth/result/event).
  ESP_LOGI(kTag, "ws msg type=%s bytes=%u", type.c_str(), static_cast<unsigned>(message.size()));

  if (type == sWs.authRequiredType) {
    sWs.authOk = false;
    sWs.ready = false;
    wsSendAuthLocked();
    return;
  }
  if (type == sWs.authOkType) {
    sWs.authOk = true;
    sWs.ready = sWs.readyType.empty() || sWs.readyType == sWs.authOkType;
    wsSendInitLocked();
    if (sWs.ready) {
      sWs.bootstrapTriggerPending = true;
    }
    // Pre-create the HTTP worker now, while the largest contiguous block is at
    // its post-TLS peak (~18KB). Value-slot allocations during the first
    // fetchAndResolve will fragment the heap, dropping largest to ~10KB. If
    // we wait until icon-fetch time the worker-creation (8KB) would push
    // largest below the HTTP client's minimum, permanently blocking icon loads.
    (void)ensureHttpWorker();
    ESP_LOGI(kTag, "ws auth_ok ready");
    return;
  }
  if (type == sWs.authInvalidType) {
    sWs.authOk = false;
    sWs.ready = false;
    return;
  }
  if (!sWs.ready && type == sWs.readyType) {
    sWs.ready = true;
    sWs.bootstrapTriggerPending = true;
    return;
  }

  if (type == "result") {
    int id = 0;
    bool success = false;
    (void)objectMemberInt(message, "id", id);
    (void)objectMemberBool(message, "success", success);
    const uint32_t reqId = static_cast<uint32_t>(id);
    auto pendingIt = sWs.pendingReqById.find(reqId);
    if (pendingIt == sWs.pendingReqById.end()) {
      // Fire-and-forget publishes (e.g. tap toggle) are not registered; log at verbose.
      ESP_LOGV(kTag, "ws result untracked id=%u success=%d", static_cast<unsigned>(reqId), success ? 1 : 0);
      return;
    }

    WsReqMeta meta = pendingIt->second;
    sWs.pendingReqById.erase(pendingIt);
    if (meta.isSubscribe) {
      sWs.cacheKeyToPendingSubscribeReq.erase(meta.cacheKey);
    } else {
      sWs.cacheKeyToPendingBootstrapReq.erase(meta.cacheKey);
    }
    if (!success) {
      ESP_LOGW(kTag, "ws result failed key=%s kind=%s id=%u", meta.cacheKey.c_str(),
               meta.isSubscribe ? "subscribe" : "bootstrap",
               static_cast<unsigned>(reqId));
      return;
    }

    if (meta.isSubscribe) {
      sWs.activeSubById[reqId] = meta;
      sWs.cacheKeyToActiveSubId[meta.cacheKey] = reqId;
      return;
    }

    std::string payloadJson;
    const std::string resultPath = meta.resultPath.empty() ? "result" : meta.resultPath;
    if (wsExtractPayloadByPath(message, resultPath, payloadJson)) {
      wsStoreCacheLocked(meta.cacheKey, payloadJson, "bootstrap", resultPath);
    } else {
      // result is null or missing — this is normal for HA render_template which
      // acts as a persistent subscription: result=null confirms registration, and
      // data arrives as events with this same id. Promote to active subscription
      // so future events are routed to the cache.
      ESP_LOGI(kTag, "ws bootstrap null-result, promoting to sub key=%s id=%u",
               meta.cacheKey.c_str(), static_cast<unsigned>(reqId));
      sWs.activeSubById[reqId] = meta;
      sWs.cacheKeyToActiveSubId[meta.cacheKey] = reqId;
    }
    return;
  }

  if (type == "event") {
    int id = 0;
    (void)objectMemberInt(message, "id", id);
    const uint32_t subId = static_cast<uint32_t>(id);
    auto subIt = sWs.activeSubById.find(subId);
    if (subIt == sWs.activeSubById.end()) {
      return;
    }

    const WsReqMeta& meta = subIt->second;
    std::string payloadJson;
    const std::string eventPath = meta.eventPath.empty() ? "event" : meta.eventPath;
    if (wsExtractPayloadByPath(message, eventPath, payloadJson)) {
      wsStoreCacheLocked(meta.cacheKey, payloadJson, "event", eventPath);
    }
    return;
  }
}

void wsClientEventHandler(void* /*arg*/, esp_event_base_t /*base*/, int32_t eventId, void* eventData) {
  if (!takeWsLock(50)) {
    return;
  }

  if (eventId == WEBSOCKET_EVENT_CONNECTED) {
    sWs.started = true;
    sWs.authOk = false;
    sWs.ready = false;
    sWs.initSent = false;
    sWs.rxFrame.clear();
  } else if (eventId == WEBSOCKET_EVENT_DISCONNECTED || eventId == WEBSOCKET_EVENT_CLOSED ||
             eventId == WEBSOCKET_EVENT_ERROR) {
    sWs.authOk = false;
    sWs.ready = false;
    sWs.initSent = false;
    sWs.started = false;
    sWs.reconnectDueMs = platform::millisMs() + 1000U;
  } else if (eventId == WEBSOCKET_EVENT_DATA) {
    auto* data = static_cast<esp_websocket_event_data_t*>(eventData);
    if (data != nullptr && data->data_ptr != nullptr && data->data_len > 0) {
      if (data->payload_offset == 0) {
        sWs.rxFrame.clear();
      }
      sWs.rxFrame.append(data->data_ptr, static_cast<size_t>(data->data_len));
      const int total = data->payload_len;
      const int offset = data->payload_offset;
      const int len = data->data_len;
      const bool done = data->fin || total <= 0 || (offset + len) >= total;
      if (done && !sWs.rxFrame.empty()) {
        wsHandleMessageLocked(std::string_view(sWs.rxFrame));
        sWs.rxFrame.clear();
      }
    }
  }

  giveWsLock();
}

bool normalizeWsUrl(const std::string& baseUrl, const std::string& wsPath, std::string& out) {
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
    if (!path.empty() && path.front() != '/') {
      path.insert(path.begin(), '/');
    }
    if (!out.empty() && out.back() == '/') {
      out.pop_back();
    }
    out += path;
  }
  return true;
}

bool parseWsProfileFromSettings(const std::string& profileName, WsProfileConfig& out) {
  out = {};
  const std::string trimmedName = trimCopy(profileName);
  if (trimmedName.empty()) {
    return false;
  }
  const std::string* profilesRaw = readSettingRef("ws_profiles");
  if (profilesRaw == nullptr) {
    return false;
  }
  const std::string_view profilesRoot = trimView(*profilesRaw);
  if (profilesRoot.empty() || profilesRoot.front() != '{') {
    return false;
  }

  std::string_view profileObj;
  if (!objectMemberObject(profilesRoot, trimmedName.c_str(), profileObj)) {
    return false;
  }

  (void)objectMemberString(profileObj, "url", out.url);
  (void)objectMemberString(profileObj, "path", out.path);
  (void)objectMemberString(profileObj, "token", out.token);
  (void)objectMemberString(profileObj, "auth_message", out.authMessage);
  (void)objectMemberString(profileObj, "auth_required_type", out.authRequiredType);
  (void)objectMemberString(profileObj, "auth_ok_type", out.authOkType);
  (void)objectMemberString(profileObj, "auth_invalid_type", out.authInvalidType);
  (void)objectMemberString(profileObj, "ready_type", out.readyType);

  std::string_view initArray;
  if (objectMemberArray(profileObj, "init", initArray)) {
    forEachArrayElement(initArray, [&](int, std::string_view value) {
      value = trimView(value);
      if (value.empty()) {
        return;
      }
      out.initMessages.push_back(valueViewToText(value));
    });
  }

  return true;
}

void teardownWsClient() {
  esp_websocket_client_handle_t client = nullptr;
  if (takeWsLock(250)) {
    client = sWs.client;
    sWs.client = nullptr;
    sWs.profileKey.clear();
    sWs.wsUrl.clear();
    sWs.tlsSkipVerify = false;
    sWs.token.clear();
    sWs.authTemplate.clear();
    sWs.authRequiredType = "auth_required";
    sWs.authOkType = "auth_ok";
    sWs.authInvalidType = "auth_invalid";
    sWs.readyType = "ready";
    sWs.initMessages.clear();
    sWs.initSent = false;
    sWs.authOk = false;
    sWs.ready = false;
    sWs.started = false;
    sWs.bootstrapTriggerPending = false;
    sWs.nextReqId = 1;
    sWs.reconnectDueMs = 0;
    sWs.failureStreak = 0;
    sWs.rxFrame.clear();
    sWs.cacheJsonByKey.clear();
    sWs.pendingReqById.clear();
    sWs.cacheKeyToPendingBootstrapReq.clear();
    sWs.cacheKeyToPendingSubscribeReq.clear();
    sWs.activeSubById.clear();
    sWs.cacheKeyToActiveSubId.clear();
    giveWsLock();
  }
  if (client != nullptr) {
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
  }
}

bool ensureWsConnected(const std::string& profileKey, const std::string& wsUrl, const std::string& token,
                       const std::string& authTemplate, const std::string& authRequiredType,
                       const std::string& authOkType, const std::string& authInvalidType,
                       const std::string& readyType, const std::vector<std::string>& initMessages,
                       const std::string& widgetId, std::string& reasonOut) {
  reasonOut.clear();
  if (wsUrl.empty()) {
    reasonOut = "ws url empty";
    return false;
  }
  if (urlUsesTls(wsUrl) && !tlsClockReady()) {
    reasonOut = "clock not synced";
    return false;
  }

  bool reinit = false;
  if (!takeWsLock(250)) {
    reasonOut = "ws lock timeout";
    return false;
  }
  if (sWs.client == nullptr || sWs.wsUrl != wsUrl || sWs.profileKey != profileKey ||
      sWs.tlsSkipVerify != s.tlsSkipVerify ||
      sWs.token != token || sWs.authTemplate != authTemplate ||
      sWs.authRequiredType != authRequiredType || sWs.authOkType != authOkType ||
      sWs.authInvalidType != authInvalidType || sWs.readyType != readyType ||
      sWs.initMessages != initMessages) {
    reinit = true;
  }
  giveWsLock();

  if (reinit) {
    // Only guard heap for TLS when actually initiating a new connection.
    if (urlUsesTls(wsUrl)) {
      size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      size_t freeHeap = esp_get_free_heap_size();
      size_t minLargest = tlsMinLargestForFreeHeap(freeHeap);
      if (largest < minLargest || freeHeap < kTlsMinFree8Bit) {
        (void)reclaimRuntimeCachesLowHeap("ws-tls-guard");
        largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        freeHeap = esp_get_free_heap_size();
        minLargest = tlsMinLargestForFreeHeap(freeHeap);
      }
      if (largest < minLargest || freeHeap < kTlsMinFree8Bit) {
        ESP_LOGE(kTag, "ws tls guard defer free=%u largest=%u min_free=%u min_largest=%u",
                 static_cast<unsigned>(freeHeap), static_cast<unsigned>(largest),
                 static_cast<unsigned>(kTlsMinFree8Bit), static_cast<unsigned>(minLargest));
        reasonOut = "ws tls low heap";
        return false;
      }
    }
    teardownWsClient();
    if (!takeWsLock(250)) {
      reasonOut = "ws lock timeout";
      return false;
    }
    sWs.profileKey = profileKey;
    sWs.wsUrl = wsUrl;
    sWs.tlsSkipVerify = s.tlsSkipVerify;
    sWs.token = token;
    sWs.authTemplate = authTemplate;
    sWs.authRequiredType = trimCopy(authRequiredType.empty() ? "auth_required" : authRequiredType);
    sWs.authOkType = trimCopy(authOkType.empty() ? "auth_ok" : authOkType);
    sWs.authInvalidType = trimCopy(authInvalidType.empty() ? "auth_invalid" : authInvalidType);
    sWs.readyType = trimCopy(readyType.empty() ? "ready" : readyType);
    sWs.initMessages = initMessages;
    sWs.initSent = false;
    sWs.authOk = false;
    sWs.ready = false;
    sWs.started = false;
    sWs.bootstrapTriggerPending = false;
    sWs.nextReqId = 1;

    esp_websocket_client_config_t cfg = {};
    cfg.uri = sWs.wsUrl.c_str();
    cfg.disable_auto_reconnect = false;
    cfg.reconnect_timeout_ms = 5000;
    cfg.network_timeout_ms = static_cast<int>(kWsConnectTimeoutMs);
    cfg.keep_alive_enable = true;
    cfg.keep_alive_idle = static_cast<int>(kWsDefaultKeepAliveMs / 1000U);
    cfg.keep_alive_interval = 10;
    cfg.keep_alive_count = 3;
    cfg.buffer_size = static_cast<int>(kWsMaxFrameBytes);
    static bool sLoggedWsTlsBuildCfg = false;
    if (!sLoggedWsTlsBuildCfg && sWs.wsUrl.rfind("wss://", 0) == 0) {
      sLoggedWsTlsBuildCfg = true;
      ESP_LOGI(kTag,
               "ws tls build cfg insecure=%d skip_verify_cfg=%d mbedtls_client_tickets=%d "
               "mbedtls_server_tickets=%d dyn_buf=%d keep_peer_cert=%d in_len=%u out_len=%u",
               kEspTlsInsecureEnabled, kEspTlsSkipServerVerifyEnabled, kMbedtlsClientSessionTicketsEnabled,
               kMbedtlsServerSessionTicketsEnabled, kMbedtlsDynamicBufferEnabled,
               kMbedtlsKeepPeerCertificateEnabled,
               static_cast<unsigned>(CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN),
               static_cast<unsigned>(CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN));
    }
    if (sWs.tlsSkipVerify) {
#if CONFIG_ESP_TLS_INSECURE && CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
      cfg.crt_bundle_attach = nullptr;
      cfg.skip_cert_common_name_check = true;
#else
      ESP_LOGW(kTag,
               "ws tls_skip_verify requested but CONFIG_ESP_TLS_INSECURE and "
               "CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY are required");
      cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    } else {
      cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    sWs.client = esp_websocket_client_init(&cfg);
    if (sWs.client == nullptr) {
      giveWsLock();
      reasonOut = "ws init failed";
      return false;
    }
    (void)esp_websocket_register_events(sWs.client, WEBSOCKET_EVENT_ANY, wsClientEventHandler, nullptr);
    ESP_LOGI(kTag, "ws connect widget=%s url=%s tls_skip_verify=%d", widgetId.c_str(), sWs.wsUrl.c_str(),
             sWs.tlsSkipVerify ? 1 : 0);
    if (esp_websocket_client_start(sWs.client) != ESP_OK) {
      esp_websocket_client_destroy(sWs.client);
      sWs.client = nullptr;
      giveWsLock();
      reasonOut = "ws start failed";
      return false;
    }
    giveWsLock();
    reasonOut = "ws not ready";
    return false;
  }

  if (!takeWsLock(50)) {
    reasonOut = "ws lock timeout";
    return false;
  }
  const bool ready = sWs.client != nullptr && sWs.ready && sWs.authOk && sWs.started;
  giveWsLock();
  reasonOut = ready ? "ws ready" : "ws not ready";
  return ready;
}

bool readWsPayloadJson(const std::string& cacheKey, std::string& payloadJson) {
  payloadJson.clear();
  if (cacheKey.empty()) {
    return false;
  }
  if (!takeWsLock(100)) {
    return false;
  }
  auto it = sWs.cacheJsonByKey.find(cacheKey);
  if (it != sWs.cacheJsonByKey.end()) {
    payloadJson = it->second;
  }
  giveWsLock();
  return !payloadJson.empty();
}

bool wsPublishMessage(const std::string& message, std::string& reasonOut) {
  reasonOut.clear();
  const std::string trimmed = trimCopy(message);
  if (trimmed.empty()) {
    reasonOut = "tap message empty";
    return false;
  }
  if (!takeWsLock(200)) {
    reasonOut = "ws lock timeout";
    return false;
  }
  if (sWs.client == nullptr || !sWs.started || !sWs.authOk || !sWs.ready) {
    giveWsLock();
    reasonOut = "ws not ready";
    return false;
  }
  const uint32_t reqId = nextWsReqIdLocked();
  std::string payload;
  if (!buildWsMessageWithId(trimmed, reqId, payload)) {
    giveWsLock();
    reasonOut = "tap message not json object";
    return false;
  }
  const bool ok = wsSendTextLocked(payload);
  giveWsLock();
  if (!ok) {
    reasonOut = "ws send failed";
    return false;
  }
  return true;
}

bool requestWsBootstrap(const std::string& cacheKey, const std::string& bootstrapTemplate,
                        const std::string& resultPath, const std::string& eventPath,
                        std::string& reasonOut) {
  reasonOut.clear();
  if (cacheKey.empty()) {
    reasonOut = "ws cache_key empty";
    return false;
  }
  const std::string payloadTemplate = trimCopy(bootstrapTemplate);
  if (payloadTemplate.empty()) {
    reasonOut = "ws bootstrap empty";
    return false;
  }

  if (!takeWsLock(200)) {
    reasonOut = "ws lock timeout";
    return false;
  }
  if (sWs.client == nullptr || !sWs.started || !sWs.authOk || !sWs.ready) {
    giveWsLock();
    reasonOut = "ws not ready";
    return false;
  }
  if (sWs.cacheKeyToPendingBootstrapReq.count(cacheKey) != 0) {
    giveWsLock();
    reasonOut = "ws bootstrap queued";
    return false;
  }
  // If this bootstrap was already promoted to an active subscription (null-result
  // path for HA render_template), don't re-register — events will keep flowing.
  if (sWs.cacheKeyToActiveSubId.count(cacheKey) != 0) {
    giveWsLock();
    reasonOut = "ws bootstrap subscribed";
    return false;
  }
  const uint32_t reqId = nextWsReqIdLocked();
  std::string payload;
  if (!buildWsMessageWithId(payloadTemplate, reqId, payload)) {
    giveWsLock();
    reasonOut = "ws bootstrap invalid";
    return false;
  }
  if (!wsSendTextLocked(payload)) {
    giveWsLock();
    reasonOut = "ws send failed";
    return false;
  }
  WsReqMeta meta = {};
  meta.cacheKey = cacheKey;
  meta.resultPath = resultPath;
  meta.eventPath = eventPath;
  meta.isSubscribe = false;
  meta.sentAtMs = platform::millisMs();
  sWs.pendingReqById[reqId] = meta;
  sWs.cacheKeyToPendingBootstrapReq[cacheKey] = reqId;
  giveWsLock();
  reasonOut = "ws bootstrap queued";
  return true;
}

bool requestWsSubscription(const std::string& cacheKey, const std::string& subscribeTemplate,
                           const std::string& resultPath, const std::string& eventPath,
                           std::string& reasonOut) {
  reasonOut.clear();
  if (cacheKey.empty()) {
    reasonOut = "ws cache_key empty";
    return false;
  }
  const std::string payloadTemplate = trimCopy(subscribeTemplate);
  if (payloadTemplate.empty()) {
    reasonOut = "ws subscribe empty";
    return false;
  }

  if (!takeWsLock(200)) {
    reasonOut = "ws lock timeout";
    return false;
  }
  if (sWs.client == nullptr || !sWs.started || !sWs.authOk || !sWs.ready) {
    giveWsLock();
    reasonOut = "ws not ready";
    return false;
  }
  if (sWs.cacheKeyToActiveSubId.count(cacheKey) != 0) {
    giveWsLock();
    reasonOut = "ws subscribed";
    return true;
  }
  if (sWs.cacheKeyToPendingSubscribeReq.count(cacheKey) != 0) {
    giveWsLock();
    reasonOut = "ws subscribe queued";
    return false;
  }

  const uint32_t reqId = nextWsReqIdLocked();
  std::string payload;
  if (!buildWsMessageWithId(payloadTemplate, reqId, payload)) {
    giveWsLock();
    reasonOut = "ws subscribe invalid";
    return false;
  }
  if (!wsSendTextLocked(payload)) {
    giveWsLock();
    reasonOut = "ws send failed";
    return false;
  }
  WsReqMeta meta = {};
  meta.cacheKey = cacheKey;
  meta.resultPath = resultPath;
  meta.eventPath = eventPath;
  meta.isSubscribe = true;
  meta.sentAtMs = platform::millisMs();
  sWs.pendingReqById[reqId] = meta;
  sWs.cacheKeyToPendingSubscribeReq[cacheKey] = reqId;
  giveWsLock();
  reasonOut = "ws subscribe queued";
  return true;
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

bool wsCallService(const std::string& domain, const std::string& service, const std::string& serviceDataJson,
                   std::string& reasonOut) {
  reasonOut.clear();
  if (domain.empty() || service.empty()) {
    reasonOut = "ws service empty";
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

  if (!takeWsLock(200)) {
    reasonOut = "ws lock timeout";
    return false;
  }
  if (sWs.client == nullptr || !sWs.started || !sWs.authOk || !sWs.ready) {
    giveWsLock();
    reasonOut = "ws not ready";
    return false;
  }

  const uint32_t reqId = nextWsReqIdLocked();
  std::string payload = "{\"id\":";
  payload += std::to_string(static_cast<unsigned>(reqId));
  payload += ",\"type\":\"call_service\",\"domain\":\"";
  payload += jsonEscape(domain);
  payload += "\",\"service\":\"";
  payload += jsonEscape(service);
  payload += "\",\"service_data\":";
  payload += body;
  payload.push_back('}');
  const bool ok = wsSendTextLocked(payload);
  giveWsLock();
  if (!ok) {
    reasonOut = "ws send failed";
    return false;
  }
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
  if (action == "ws" || action == "ws_publish" || action == "websocket") {
    return TapActionType::kWsPublish;
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
  if (s.tapAction != TapActionType::kHttp &&
      s.tapAction != TapActionType::kWsPublish) {
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

  // For generic websocket sources, prefer existing shared WS connection over HTTPS taps.
  if (s.source == DataSource::kWebSocket && s.tapAction == TapActionType::kHttp) {
    s.tapAction = TapActionType::kWsPublish;
  }
}

bool httpRequestDirect(const std::string& method, const std::string& url,
                       const std::vector<KeyValue>& headers, const std::string& reqBody, int& statusCode,
                       std::string& body, std::string& reason, uint32_t& durationMs, std::string& hostOut,
                       bool& viaProxy, uint32_t maxResponseBytes, bool tlsSkipVerify) {
  statusCode = 0;
  body.clear();
  reason.clear();
  durationMs = 0;
  hostOut = hostFromUrl(url);
  viaProxy = isProxyUrl(url, hostOut);
  if (urlUsesTls(url) && !tlsClockReady()) {
    reason = "clock not synced";
    return false;
  }
  if (urlUsesTls(url)) {
    static bool sLoggedTlsGuardConfig = false;
    if (!sLoggedTlsGuardConfig) {
      sLoggedTlsGuardConfig = true;
      ESP_LOGI(kTag, "tls guard cfg min_free=%u min_largest=%u relaxed_largest=%u relaxed_free=%u",
               static_cast<unsigned>(kTlsMinFree8Bit), static_cast<unsigned>(kTlsMinLargest8Bit),
               static_cast<unsigned>(kTlsRelaxedLargest8Bit),
               static_cast<unsigned>(kTlsRelaxedFreeThreshold8Bit));
      ESP_LOGI(kTag,
               "tls build cfg insecure=%d skip_verify_cfg=%d mbedtls_client_tickets=%d "
               "mbedtls_server_tickets=%d dyn_buf=%d keep_peer_cert=%d in_len=%u out_len=%u",
               kEspTlsInsecureEnabled, kEspTlsSkipServerVerifyEnabled, kMbedtlsClientSessionTicketsEnabled,
               kMbedtlsServerSessionTicketsEnabled, kMbedtlsDynamicBufferEnabled,
               kMbedtlsKeepPeerCertificateEnabled,
               static_cast<unsigned>(CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN),
               static_cast<unsigned>(CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN));
    }
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t freeHeap = esp_get_free_heap_size();
    size_t minLargest = tlsMinLargestForFreeHeap(freeHeap);
    if (largest < minLargest || freeHeap < kTlsMinFree8Bit) {
      (void)reclaimRuntimeCachesLowHeap("tls-guard");
      largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      freeHeap = esp_get_free_heap_size();
      minLargest = tlsMinLargestForFreeHeap(freeHeap);
    }
    if (largest < minLargest || freeHeap < kTlsMinFree8Bit) {
      char lowHeapReason[96];
      std::snprintf(lowHeapReason, sizeof(lowHeapReason), "tls low heap free=%u largest=%u",
                    static_cast<unsigned>(freeHeap), static_cast<unsigned>(largest));
      reason = lowHeapReason;
      ESP_LOGE(kTag, "tls guard defer host=%s free=%u largest=%u min_free=%u min_largest=%u",
               hostOut.c_str(), static_cast<unsigned>(freeHeap), static_cast<unsigned>(largest),
               static_cast<unsigned>(kTlsMinFree8Bit), static_cast<unsigned>(minLargest));
      return false;
    }
  }

  if (!http_transport_gate::take(kHttpGateTimeoutMs)) {
    reason = "transport gate timeout";
    return false;
  }

  const uint32_t startMs = platform::millisMs();
  ESP_LOGD(kTag, "http start method=%s host=%s proxy=%d url=%s", method.c_str(), hostOut.c_str(),
           viaProxy ? 1 : 0, url.c_str());

  HttpCapture cap;
  cap.maxBytes = std::clamp<uint32_t>(maxResponseBytes, kHttpResponseMaxBytesMin, kHttpResponseMaxBytesMax);
  int64_t responseContentLength = -1;
  int chunkedResponse = 0;
  auto configureClientRequest = [&](esp_http_client_handle_t h) {
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
    esp_http_client_set_method(h, m);
    esp_http_client_set_header(h, "Accept", "application/json");
    esp_http_client_set_header(h, "User-Agent", "CoStar-ESP32/1.0");
    esp_http_client_set_header(h, "Accept-Encoding", "identity");
    if (!reqBody.empty()) {
      esp_http_client_set_header(h, "Content-Type", "application/json");
      esp_http_client_set_post_field(h, reqBody.c_str(), static_cast<int>(reqBody.size()));
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
      esp_http_client_set_header(h, key.c_str(), value.c_str());
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
  };

  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.timeout_ms = static_cast<int>(kHttpTimeoutMs);
  if (urlUsesTls(url)) {
    cfg.tls_version = ESP_HTTP_CLIENT_TLS_VER_ANY;
  }
  if (tlsSkipVerify) {
#if CONFIG_ESP_TLS_INSECURE && CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
    cfg.crt_bundle_attach = nullptr;
    cfg.skip_cert_common_name_check = true;
#else
    ESP_LOGW(kTag,
             "tls_skip_verify requested but CONFIG_ESP_TLS_INSECURE and "
             "CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY are required");
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif
  } else {
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
  }
  cfg.disable_auto_redirect = false;
  cfg.max_redirection_count = 5;
  cfg.keep_alive_enable = false;
  if (method == "GET") {
    cfg.event_handler = nullptr;
    cfg.user_data = nullptr;
  } else {
    cfg.event_handler = httpEventHandler;
    cfg.user_data = &cap;
  }
  cfg.buffer_size = 1024;
  cfg.buffer_size_tx = 512;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) {
    http_transport_gate::give();
    reason = "http init failed";
    return false;
  }
  configureClientRequest(client);

  esp_err_t reqErr = ESP_OK;
  if (method == "GET") {
    reqErr = esp_http_client_open(client, 0);
    if (reqErr == ESP_OK) {
      const int64_t contentLen = esp_http_client_fetch_headers(client);
      responseContentLength = contentLen;
      statusCode = esp_http_client_get_status_code(client);
      chunkedResponse = esp_http_client_is_chunked_response(client) ? 1 : 0;
      if (contentLen > 0) {
        if (static_cast<size_t>(contentLen) > cap.maxBytes) {
          cap.overflow = true;
        } else {
          (void)ensureHttpCaptureCapacity(&cap, static_cast<size_t>(contentLen));
        }
      }
      int zeroReadStreak = 0;
      while (reqErr == ESP_OK) {
        char tmp[512];
        const int got = esp_http_client_read(client, tmp, sizeof(tmp));
        if (got == -ESP_ERR_HTTP_EAGAIN) {
          if (++zeroReadStreak >= 5) {
            reqErr = ESP_FAIL;
            reason = "http read timeout";
            break;
          }
          vTaskDelay(pdMS_TO_TICKS(20));
          continue;
        }
        if (got < 0) {
          reqErr = ESP_FAIL;
          reason = "http read failed";
          break;
        }
        if (got == 0) {
          if (esp_http_client_is_complete_data_received(client)) {
            break;
          }
          if (++zeroReadStreak >= 5) {
            reqErr = ESP_FAIL;
            reason = "http read incomplete";
            break;
          }
          vTaskDelay(pdMS_TO_TICKS(20));
          continue;
        }
        zeroReadStreak = 0;
        const size_t needBytes = cap.size + static_cast<size_t>(got);
        if (!ensureHttpCaptureCapacity(&cap, needBytes)) {
          break;
        }
        std::memcpy(cap.data + cap.size, tmp, static_cast<size_t>(got));
        cap.size += static_cast<size_t>(got);
        cap.data[cap.size] = '\0';
      }
    }
  } else {
    reqErr = esp_http_client_perform(client);
    if (reqErr == ESP_OK) {
      statusCode = esp_http_client_get_status_code(client);
      responseContentLength = esp_http_client_get_content_length(client);
      chunkedResponse = esp_http_client_is_chunked_response(client) ? 1 : 0;
    }
  }

  // Release TLS/client allocations before attempting to allocate std::string body storage.
  esp_http_client_cleanup(client);
  client = nullptr;

  http_transport_gate::give();
  durationMs = platform::millisMs() - startMs;

  if (reqErr != ESP_OK) {
    logTlsFailureEpoch(url, reqErr);
    if (reason.empty()) {
      reason = esp_err_to_name(reqErr);
    }
    ESP_LOGW(kTag, "http fail host=%s proxy=%d dur_ms=%u reason=%s", hostOut.c_str(), viaProxy ? 1 : 0,
             static_cast<unsigned>(durationMs), reason.c_str());
    if (cap.data != nullptr) {
      heap_caps_free(cap.data);
      cap.data = nullptr;
    }
    return false;
  }
  if (statusCode <= 0) {
    reason = "no-http-status";
    ESP_LOGW(kTag, "http fail host=%s proxy=%d dur_ms=%u reason=%s", hostOut.c_str(), viaProxy ? 1 : 0,
             static_cast<unsigned>(durationMs), reason.c_str());
    if (cap.data != nullptr) {
      heap_caps_free(cap.data);
      cap.data = nullptr;
    }
    return false;
  }
  if (cap.overflow) {
    reason = "http body too large";
    ESP_LOGW(kTag, "http fail host=%s proxy=%d dur_ms=%u reason=%s max_bytes=%u", hostOut.c_str(),
             viaProxy ? 1 : 0, static_cast<unsigned>(durationMs), reason.c_str(),
             static_cast<unsigned>(cap.maxBytes));
    if (cap.data != nullptr) {
      heap_caps_free(cap.data);
      cap.data = nullptr;
    }
    return false;
  }
  if (cap.oom) {
    reason = "http body capture oom";
    ESP_LOGW(kTag, "http fail host=%s proxy=%d dur_ms=%u reason=%s max_bytes=%u", hostOut.c_str(),
             viaProxy ? 1 : 0, static_cast<unsigned>(durationMs), reason.c_str(),
             static_cast<unsigned>(cap.maxBytes));
    if (cap.data != nullptr) {
      heap_caps_free(cap.data);
      cap.data = nullptr;
    }
    return false;
  }
  if (cap.size > 0 && cap.data != nullptr) {
    const size_t largestBeforeCopy = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largestBeforeCopy < (cap.size + 1024U)) {
      reason = "http body copy oom";
      ESP_LOGW(kTag,
               "http fail host=%s proxy=%d dur_ms=%u reason=%s body=%u largest=%u",
               hostOut.c_str(), viaProxy ? 1 : 0, static_cast<unsigned>(durationMs), reason.c_str(),
               static_cast<unsigned>(cap.size), static_cast<unsigned>(largestBeforeCopy));
      if (cap.data != nullptr) {
        heap_caps_free(cap.data);
        cap.data = nullptr;
      }
      return false;
    }
    body.assign(cap.data, cap.size);
  }
  if (cap.data != nullptr) {
    heap_caps_free(cap.data);
    cap.data = nullptr;
  }

  if (statusCode >= 200 && statusCode < 300 && body.empty()) {
    ESP_LOGW(kTag,
             "http empty body host=%s proxy=%d status=%d content_len=%lld chunked=%d dur_ms=%u",
             hostOut.c_str(), viaProxy ? 1 : 0, statusCode,
             static_cast<long long>(responseContentLength), chunkedResponse,
             static_cast<unsigned>(durationMs));
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
    if (xQueueReceive(sHttpJobQueue, &job, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    if (job == nullptr) {
      break;
    }
    HttpResult* result = new HttpResult();
    if (result == nullptr) {
      delete job;
      continue;
    }
    result->ok =
        httpRequestDirect(job->method, job->url, job->headers, job->body, result->statusCode, result->body,
                          result->reason, result->durationMs, result->host, result->viaProxy,
                          job->maxResponseBytes, job->tlsSkipVerify);
    if (job->replyQueue != nullptr) {
      if (xQueueSend(job->replyQueue, &result, pdMS_TO_TICKS(100)) != pdTRUE) {
        delete result;
      }
    } else {
      delete result;
    }
    delete job;
  }
  sHttpWorkerTask = nullptr;
  vTaskDelete(nullptr);
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

void teardownHttpWorker() {
  if (sHttpJobQueue == nullptr) {
    return;
  }

  // Stop the worker task so we can reclaim its stack between layout switches.
  if (sHttpWorkerTask != nullptr) {
    HttpJob* quit = nullptr;
    (void)xQueueSend(sHttpJobQueue, &quit, pdMS_TO_TICKS(20));
    for (int i = 0; i < 40 && sHttpWorkerTask != nullptr; ++i) {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (sHttpWorkerTask != nullptr) {
      vTaskDelete(sHttpWorkerTask);
      sHttpWorkerTask = nullptr;
    }
  }

  // Drain any queued jobs and notify waiters with nullptr (treated as failure).
  HttpJob* job = nullptr;
  while (xQueueReceive(sHttpJobQueue, &job, 0) == pdTRUE) {
    if (job != nullptr) {
      if (job->replyQueue != nullptr) {
        HttpResult* nullResult = nullptr;
        (void)xQueueSend(job->replyQueue, &nullResult, 0);
      }
      delete job;
    }
  }
  vQueueDelete(sHttpJobQueue);
  sHttpJobQueue = nullptr;
}

void settleAfterNetworkTeardown() {
  const size_t freeBefore = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t largestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  // Allow async socket/task teardown paths to complete before next layout starts
  // issuing fresh TLS connections.
  for (int i = 0; i < 3; ++i) {
    vTaskDelay(pdMS_TO_TICKS(25));
  }
  (void)reclaimRuntimeCachesLowHeap("layout-switch");
  const size_t freeAfter = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t largestAfter = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  ESP_LOGI(kTag, "reset settle net free=%u->%u largest=%u->%u", static_cast<unsigned>(freeBefore),
           static_cast<unsigned>(freeAfter), static_cast<unsigned>(largestBefore),
           static_cast<unsigned>(largestAfter));
}

bool httpGetViaWorker(std::string&& url, std::vector<KeyValue>&& headers, int& statusCode, std::string& body,
                      std::string& reason) {
  statusCode = 0;
  body.clear();
  reason.clear();

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
  job->url = std::move(url);
  job->method = "GET";
  job->body.clear();
  job->headers = std::move(headers);
  job->maxResponseBytes = s.httpMaxBytes;
  job->tlsSkipVerify = s.tlsSkipVerify;
  job->replyQueue = replyQueue;

  if (xQueueSend(sHttpJobQueue, &job, pdMS_TO_TICKS(kHttpGateTimeoutMs)) != pdTRUE) {
    delete job;
    vQueueDelete(replyQueue);
    reason = "http worker queue full";
    return false;
  }

  HttpResult* result = nullptr;
  BaseType_t got = xQueueReceive(replyQueue, &result, pdMS_TO_TICKS(kHttpWorkerReplyTimeoutMs));
  if (got != pdTRUE || result == nullptr) {
    // Avoid queue lifetime race with httpWorkerTask on slow network paths.
    got = xQueueReceive(replyQueue, &result, pdMS_TO_TICKS(kHttpTimeoutMs + 4000U));
  }
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

bool httpGet(const std::string& url, const std::vector<KeyValue>& headers, int& statusCode,
             std::string& body, std::string& reason) {
  statusCode = 0;
  body.clear();
  reason.clear();

  if (!ensureHttpWorker()) {
    uint32_t durationMs = 0;
    std::string host;
    bool viaProxy = false;
    return httpRequestDirect("GET", url, headers, "", statusCode, body, reason, durationMs, host, viaProxy,
                             s.httpMaxBytes, s.tlsSkipVerify);
  }

  std::string urlCopy = url;
  std::vector<KeyValue> headersCopy = headers;
  return httpGetViaWorker(std::move(urlCopy), std::move(headersCopy), statusCode, body, reason);
}

bool httpGet(std::string&& url, std::vector<KeyValue>&& headers, int& statusCode, std::string& body,
             std::string& reason) {
  statusCode = 0;
  body.clear();
  reason.clear();

  if (!ensureHttpWorker()) {
    uint32_t durationMs = 0;
    std::string host;
    bool viaProxy = false;
    return httpRequestDirect("GET", url, headers, "", statusCode, body, reason, durationMs, host, viaProxy,
                             s.httpMaxBytes, s.tlsSkipVerify);
  }

  return httpGetViaWorker(std::move(url), std::move(headers), statusCode, body, reason);
}

bool httpRequestViaWorker(std::string&& method, std::string&& url, std::vector<KeyValue>&& headers,
                          std::string&& reqBody, int& statusCode, std::string& body, std::string& reason) {
  statusCode = 0;
  body.clear();
  reason.clear();

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
  job->method = std::move(method);
  job->url = std::move(url);
  job->body = std::move(reqBody);
  job->headers = std::move(headers);
  job->maxResponseBytes = s.httpMaxBytes;
  job->tlsSkipVerify = s.tlsSkipVerify;
  job->replyQueue = replyQueue;

  if (xQueueSend(sHttpJobQueue, &job, pdMS_TO_TICKS(kHttpGateTimeoutMs)) != pdTRUE) {
    delete job;
    vQueueDelete(replyQueue);
    reason = "http worker queue full";
    return false;
  }

  HttpResult* result = nullptr;
  BaseType_t got = xQueueReceive(replyQueue, &result, pdMS_TO_TICKS(kHttpWorkerReplyTimeoutMs));
  if (got != pdTRUE || result == nullptr) {
    // Avoid queue lifetime race with httpWorkerTask on slow network paths.
    got = xQueueReceive(replyQueue, &result, pdMS_TO_TICKS(kHttpTimeoutMs + 4000U));
  }
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
                             viaProxy, s.httpMaxBytes, s.tlsSkipVerify);
  }

  std::string methodCopy = method;
  std::string urlCopy = url;
  std::vector<KeyValue> headersCopy = headers;
  std::string bodyCopy = reqBody;
  return httpRequestViaWorker(std::move(methodCopy), std::move(urlCopy), std::move(headersCopy),
                              std::move(bodyCopy), statusCode, body, reason);
}

bool httpRequest(std::string&& method, std::string&& url, std::vector<KeyValue>&& headers,
                 std::string&& reqBody, int& statusCode, std::string& body, std::string& reason) {
  statusCode = 0;
  body.clear();
  reason.clear();

  if (!ensureHttpWorker()) {
    uint32_t durationMs = 0;
    std::string host;
    bool viaProxy = false;
    return httpRequestDirect(method, url, headers, reqBody, statusCode, body, reason, durationMs, host,
                             viaProxy, s.httpMaxBytes, s.tlsSkipVerify);
  }

  return httpRequestViaWorker(std::move(method), std::move(url), std::move(headers), std::move(reqBody),
                              statusCode, body, reason);
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

void noteFetchDeferred(uint32_t nowMs, const char* reason) {
  const uint32_t desiredDelayMs = std::clamp<uint32_t>(s.pollMs / 3U, 1000U, 5000U);
  if (s.backoffUntilMs == 0 || static_cast<int32_t>(s.backoffUntilMs - nowMs) < static_cast<int32_t>(desiredDelayMs)) {
    s.backoffUntilMs = nowMs + desiredDelayMs;
  }
  if (s.lastDeferredLogMs == 0 || static_cast<int32_t>(nowMs - s.lastDeferredLogMs) >= 2000) {
    ESP_LOGI(kTag, "fetch deferred widget=%s backoff_ms=%u reason=%s", s.widgetId.c_str(),
             static_cast<unsigned>(desiredDelayMs), reason != nullptr ? reason : "unknown");
    s.lastDeferredLogMs = nowMs;
  }
}

void noteFetchSuccess() {
  s.failureStreak = 0;
  s.backoffUntilMs = 0;
  s.lastDeferredLogMs = 0;
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

  if (s.tapAction == TapActionType::kWsPublish) {
    const std::string url = bindRuntimeTemplate(s.tapUrlTemplate);
    std::string domain;
    std::string service;
    if (!url.empty() && parseHaServiceFromUrl(url, domain, service)) {
      const std::string body = bindRuntimeTemplate(s.tapBodyTemplate);
      return wsCallService(domain, service, body, reasonOut);
    }
    const std::string msg = bindRuntimeTemplatePreserveUnknown(s.tapBodyTemplate);
    return wsPublishMessage(msg, reasonOut);
  }

  if (s.tapAction != TapActionType::kHttp) {
    reasonOut = "unsupported tap action";
    return false;
  }

  std::string url = bindRuntimeTemplate(s.tapUrlTemplate);
  if (url.empty()) {
    reasonOut = "tap_url empty";
    return false;
  }
  std::string method = s.tapMethod.empty() ? "POST" : s.tapMethod;
  std::transform(method.begin(), method.end(), method.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  std::string body = bindRuntimeTemplate(s.tapBodyTemplate);

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
  if (!httpRequest(std::move(method), std::move(url), std::move(headers), std::move(body), status, resp,
                   reason)) {
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
    resolvedFmt.valueMapLookup = bindRuntimeTemplate(resolvedFmt.valueMapLookup);
    std::string mappedLookupValue;
    if (resolveMappedLookupValue(resolvedFmt, raw, numeric, numericValue, mappedLookupValue)) {
      (void)setValue(field.key, mappedLookupValue);
      s.numericValues.erase(field.key);
      ++resolved;
      continue;
    }

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

bool resolveFieldsFromHttp(const std::string& jsonText) {
  if (s.retainSourceJson) {
    s.sourceJson = jsonText;
    return resolveFieldsFromJsonView(std::string_view(s.sourceJson));
  }
  s.sourceJson.clear();
  return resolveFieldsFromJsonView(std::string_view(jsonText));
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
    resolvedFmt.valueMapLookup = bindRuntimeTemplate(resolvedFmt.valueMapLookup);
    std::string mappedLookupValue;
    if (resolveMappedLookupValue(resolvedFmt, raw, numeric, numericValue, mappedLookupValue)) {
      (void)setValue(field.key, mappedLookupValue);
      s.numericValues.erase(field.key);
      ++resolved;
      continue;
    }

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

void prefetchRemoteIconsForCurrentState();

bool fetchAndResolve(uint32_t nowMs) {
  if (s.source == DataSource::kLocalTime) {
    if (!resolveFieldsFromLocalTime()) {
      noteFetchFailure(nowMs, "local_time unavailable");
      return false;
    }
    prefetchRemoteIconsForCurrentState();
    noteFetchSuccess();
    return true;
  }

  if (s.source == DataSource::kWebSocket) {
    // Guard against heap exhaustion before allocating template strings and WS
    // client buffers. A failed TLS reconnect loop can leave heap fragmented;
    // deferring here prevents std::bad_alloc / abort() deep in template binding.
    // Once the WS connection is established the TLS context is already resident
    // in heap and won't go away — apply only a modest guard for string allocs.
    {
      // Use a longer lock timeout here — the first tick after auth_ok can race
      // with the WS event handler still holding the lock. If we fail to read
      // the connected state and fall back to the TLS guard (20KB/40KB), we'll
      // incorrectly defer on a heap level that's permanently below that threshold.
      bool wsAlreadyConnected = false;
      if (takeWsLock(100)) {
        wsAlreadyConnected = sWs.client != nullptr && sWs.ready && sWs.authOk && sWs.started;
        giveWsLock();
      }
      const size_t wsMinLargest = wsAlreadyConnected ? kWsActiveMinLargest8Bit : kTlsMinLargest8Bit;
      const size_t wsMinFree = wsAlreadyConnected ? kWsActiveMinFree8Bit : kTlsMinFree8Bit;
      size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      size_t freeHeap = esp_get_free_heap_size();
      if (largest < wsMinLargest || freeHeap < wsMinFree) {
        (void)reclaimRuntimeCachesLowHeap("ws-fetch-guard");
        largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        freeHeap = esp_get_free_heap_size();
      }
      if (largest < wsMinLargest || freeHeap < wsMinFree) {
        noteFetchDeferred(nowMs, "ws low heap");
        return false;
      }
    }
    const std::string profileName =
        bindRuntimeTemplate(s.wsConnectionProfileTemplate.empty() ? readSetting("connection_profile", "")
                                                                  : s.wsConnectionProfileTemplate);
    WsProfileConfig profile = {};
    const bool hasProfile = parseWsProfileFromSettings(profileName, profile);

    const std::string cacheKey = bindRuntimeTemplate(
        s.wsCacheKeyTemplate.empty() ? s.widgetId : s.wsCacheKeyTemplate);
    const std::string bootstrapTemplate = bindRuntimeTemplatePreserveUnknown(s.wsBootstrapTemplate);
    const std::string subscribeTemplate = bindRuntimeTemplatePreserveUnknown(s.wsSubscribeTemplate);
    const std::string resultPath = bindRuntimeTemplate(s.wsResultPath);
    const std::string eventPath = bindRuntimeTemplate(s.wsEventPath);
    const std::string token = bindRuntimeTemplate(
        s.wsTokenTemplate.empty()
            ? (hasProfile ? profile.token : readSetting("ws_token", ""))
            : s.wsTokenTemplate);
    const std::string authTemplate = bindRuntimeTemplate(
        s.wsAuthTemplate.empty()
            ? (hasProfile ? profile.authMessage : readSetting("ws_auth_template", ""))
            : s.wsAuthTemplate);
    const std::string wsBase = bindRuntimeTemplate(
        s.wsUrlTemplate.empty()
            ? (hasProfile ? profile.url : readSetting("ws_url", ""))
            : s.wsUrlTemplate);
    const std::string wsPath = bindRuntimeTemplate(
        s.wsPathTemplate.empty() ? (hasProfile ? profile.path : readSetting("ws_path", ""))
                                 : s.wsPathTemplate);
    const std::string authRequiredType =
        bindRuntimeTemplate(hasProfile ? profile.authRequiredType : readSetting("ws_auth_required_type", "auth_required"));
    const std::string authOkType =
        bindRuntimeTemplate(hasProfile ? profile.authOkType : readSetting("ws_auth_ok_type", "auth_ok"));
    const std::string authInvalidType =
        bindRuntimeTemplate(hasProfile ? profile.authInvalidType : readSetting("ws_auth_invalid_type", "auth_invalid"));
    const std::string readyType =
        bindRuntimeTemplate(hasProfile ? profile.readyType : readSetting("ws_ready_type", "ready"));
    std::vector<std::string> initMessages = profile.initMessages;
    for (std::string& msg : initMessages) {
      msg = bindRuntimeTemplatePreserveUnknown(msg);
    }

    std::string wsUrl;
    if (!normalizeWsUrl(wsBase, wsPath, wsUrl)) {
      noteFetchDeferred(nowMs, "ws url invalid");
      return false;
    }

    std::string wsReason;
    const bool wsReady =
        ensureWsConnected(profileName, wsUrl, token, authTemplate, authRequiredType, authOkType,
                          authInvalidType, readyType, initMessages, s.widgetId, wsReason);
    std::string payloadJson;
    if (!readWsPayloadJson(cacheKey, payloadJson)) {
      if (!wsReady) {
        noteFetchDeferred(nowMs, wsReason.c_str());
      } else {
        std::string subscribeReason;
        (void)requestWsSubscription(cacheKey, subscribeTemplate, resultPath, eventPath, subscribeReason);
        std::string bootstrapReason;
        (void)requestWsBootstrap(cacheKey, bootstrapTemplate, resultPath, eventPath, bootstrapReason);
        if (!trimCopy(bootstrapTemplate).empty()) {
          noteFetchDeferred(nowMs, bootstrapReason.c_str());
        } else if (!trimCopy(subscribeTemplate).empty()) {
          noteFetchDeferred(nowMs, subscribeReason.c_str());
        } else {
          noteFetchDeferred(nowMs, "ws no bootstrap/subscribe");
        }
      }
      return false;
    }

    if (wsReady) {
      std::string ignoredReason;
      (void)requestWsSubscription(cacheKey, subscribeTemplate, resultPath, eventPath, ignoredReason);
    }
    if (!resolveFieldsFromHttp(std::move(payloadJson))) {
      const size_t previewLen = std::min<size_t>(payloadJson.size(), 160U);
      ESP_LOGW(kTag, "ws parse unresolved key=%s bytes=%u preview=%.*s", cacheKey.c_str(),
               static_cast<unsigned>(payloadJson.size()), static_cast<int>(previewLen),
               payloadJson.c_str());
      noteFetchFailure(nowMs, "ws parse unresolved");
      return false;
    }
    prefetchRemoteIconsForCurrentState();
    noteFetchSuccess();
    return true;
  }

  if (s.source != DataSource::kHttp) {
    noteFetchFailure(nowMs, "unsupported source");
    return false;
  }

  std::string url = bindRuntimeTemplate(s.urlTemplate);
  const bool requestUsesTls = urlUsesTls(url);
  if (s.tlsRetryGuard && requestUsesTls) {
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const size_t freeHeap = esp_get_free_heap_size();
    const size_t minLargest = tlsMinLargestForFreeHeap(freeHeap);
    if (largest < minLargest || freeHeap < kTlsMinFree8Bit) {
      noteFetchDeferred(nowMs, "tls retry guard wait heap recovery");
      return false;
    }
    s.tlsRetryGuard = false;
    ESP_LOGI(kTag, "tls retry guard cleared widget=%s free=%u largest=%u", s.widgetId.c_str(),
             static_cast<unsigned>(freeHeap), static_cast<unsigned>(largest));
  }

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
  const std::string cacheKey = buildHttpReuseKey(url, resolvedHeaders, s.httpMaxBytes);
  const SharedHttpResponse* shared = nullptr;
  bool requestOk = false;
  shared = readHttpReuseCache(cacheKey, nowMs);
  if (shared != nullptr) {
    requestOk = shared->ok;
    statusCode = shared->statusCode;
    reason = shared->reason;
    const uint32_t ageMs = nowMs - shared->fetchedAtMs;
    ESP_LOGI(kTag, "fetch http reuse widget=%s age_ms=%u ok=%d status=%d bytes=%u",
             s.widgetId.c_str(), static_cast<unsigned>(ageMs), requestOk ? 1 : 0, statusCode,
             static_cast<unsigned>(shared->body.size()));
  } else {
    ESP_LOGI(kTag, "fetch http widget=%s url=%s", s.widgetId.c_str(), url.c_str());
    requestOk = httpGet(std::move(url), std::move(resolvedHeaders), statusCode, body, reason);
    if (requestOk && statusCode >= 200 && statusCode < 300 && !body.empty()) {
      SharedHttpResponse cached = {};
      cached.ok = true;
      cached.statusCode = statusCode;
      cached.body = std::move(body);
      cached.reason.clear();
      cached.fetchedAtMs = nowMs;
      writeHttpReuseCache(cacheKey, std::move(cached));
      shared = readHttpReuseCache(cacheKey, nowMs);
    }
  }

  if (!requestOk) {
    if (requestUsesTls && reason == "ESP_ERR_HTTP_CONNECT") {
      const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      const size_t freeHeap = esp_get_free_heap_size();
      const size_t minLargest = tlsMinLargestForFreeHeap(freeHeap);
      if (largest < minLargest || freeHeap < kTlsMinFree8Bit) {
        s.tlsRetryGuard = true;
        noteFetchDeferred(nowMs, "tls connect failed; waiting for heap recovery");
        return false;
      }
    }
    noteFetchFailure(nowMs, reason.c_str());
    return false;
  }
  if (statusCode < 200 || statusCode >= 300) {
    char statusBuf[32];
    std::snprintf(statusBuf, sizeof(statusBuf), "status=%d", statusCode);
    noteFetchFailure(nowMs, statusBuf);
    return false;
  }

  const std::string* parseBody = (shared != nullptr) ? &shared->body : &body;
  if (parseBody->empty()) {
    char emptyBuf[64];
    std::snprintf(emptyBuf, sizeof(emptyBuf), "empty body status=%d", statusCode);
    noteFetchFailure(nowMs, emptyBuf);
    return false;
  }

  if (!resolveFieldsFromHttp(*parseBody)) {
    noteFetchFailure(nowMs, "dsl parse unresolved");
    return false;
  }

  prefetchRemoteIconsForCurrentState();
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
    for (int row = 0; row < 8; ++row) {
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

void drawLineThick(int x0, int y0, int x1, int y1, int thickness, uint16_t color) {
  if (thickness <= 1) {
    drawLine(x0, y0, x1, y1, color);
    return;
  }
  const int brush = std::max(2, thickness);
  const int half = brush / 2;
  int dx = std::abs(x1 - x0);
  int sx = x0 < x1 ? 1 : -1;
  int dy = -std::abs(y1 - y0);
  int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  while (true) {
    drawSolidRect(x0 - half, y0 - half, brush, brush, color);
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
  std::string localPath = path;
  if (localPath.rfind("/littlefs/", 0) == 0) {
    localPath = localPath.substr(9);
  } else if (localPath == "/littlefs") {
    localPath = "/";
  }
  if (!localPath.empty() && localPath.front() != '/') {
    localPath.insert(localPath.begin(), '/');
  }

  std::string fullPath = localPath;
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

bool loadBitmap1(const std::string& path, int w, int h, std::vector<uint8_t>& outBits) {
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
  const size_t stride = static_cast<size_t>((w + 7) / 8);
  const size_t needBytes = stride * static_cast<size_t>(h);
  outBits.assign(needBytes, 0);
  const size_t got = std::fread(outBits.data(), 1, needBytes, fp);
  std::fclose(fp);
  return got == needBytes;
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

struct IconFileCacheEntry {
  std::string path;
  size_t bytes = 0;
  std::time_t mtime = 0;
};

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

bool gatherIconFileCacheEntries(std::vector<IconFileCacheEntry>& out, size_t& totalBytes) {
  out.clear();
  totalBytes = 0;
  DIR* dir = ::opendir(kIconCacheDir);
  if (dir == nullptr) {
    return false;
  }
  while (dirent* de = ::readdir(dir)) {
    if (de->d_name[0] == '.') {
      continue;
    }
    const std::string name(de->d_name);
    if (name.size() < 4 || name.rfind(".raw") != name.size() - 4) {
      continue;
    }
    const std::string path = std::string(kIconCacheDir) + "/" + name;
    struct stat st = {};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
      continue;
    }
    IconFileCacheEntry entry;
    entry.path = path;
    entry.bytes = static_cast<size_t>(st.st_size >= 0 ? st.st_size : 0);
    entry.mtime = st.st_mtime;
    totalBytes += entry.bytes;
    out.push_back(std::move(entry));
  }
  ::closedir(dir);
  return true;
}

bool enforceIconFileCacheBudget(const std::string& keepPath, size_t incomingBytes) {
  if (incomingBytes == 0 || incomingBytes > kIconFileCacheBudgetBytes) {
    return false;
  }
  std::vector<IconFileCacheEntry> entries;
  size_t totalBytes = 0;
  if (!gatherIconFileCacheEntries(entries, totalBytes)) {
    return false;
  }

  size_t keepExistingBytes = 0;
  for (const auto& e : entries) {
    if (e.path == keepPath) {
      keepExistingBytes = e.bytes;
      break;
    }
  }
  size_t projected = totalBytes - keepExistingBytes + incomingBytes;
  if (projected <= kIconFileCacheBudgetBytes && entries.size() <= kIconFileCacheMaxEntries) {
    return true;
  }

  std::sort(entries.begin(), entries.end(), [](const IconFileCacheEntry& a, const IconFileCacheEntry& b) {
    if (a.mtime != b.mtime) {
      return a.mtime < b.mtime;
    }
    return a.path < b.path;
  });

  size_t evictedFiles = 0;
  size_t evictedBytes = 0;
  for (const auto& e : entries) {
    if (projected <= kIconFileCacheBudgetBytes &&
        (entries.size() - evictedFiles) <= kIconFileCacheMaxEntries) {
      break;
    }
    if (e.path == keepPath) {
      continue;
    }
    if (::unlink(e.path.c_str()) != 0) {
      continue;
    }
    projected = projected > e.bytes ? projected - e.bytes : 0;
    ++evictedFiles;
    evictedBytes += e.bytes;
  }

  if (evictedFiles > 0) {
    ESP_LOGI(kTag, "icon file cache evict files=%u bytes=%u projected=%u budget=%u entries=%u",
             static_cast<unsigned>(evictedFiles), static_cast<unsigned>(evictedBytes),
             static_cast<unsigned>(projected), static_cast<unsigned>(kIconFileCacheBudgetBytes),
             static_cast<unsigned>(kIconFileCacheMaxEntries));
  }

  return projected <= kIconFileCacheBudgetBytes &&
         (entries.size() - evictedFiles) <= kIconFileCacheMaxEntries;
}

bool iconMemCacheGetPtr(const std::string& key, const std::vector<uint16_t>*& outPixels) {
  outPixels = nullptr;
  auto it = sIconMemCache.find(key);
  if (it == sIconMemCache.end()) {
    return false;
  }
  it->second.lastUsedMs = platform::millisMs();
  outPixels = &it->second.pixels;
  return true;
}

void pruneIconRetryMap(uint32_t nowMs) {
  for (auto it = sIconRetryAfterMs.begin(); it != sIconRetryAfterMs.end();) {
    if (static_cast<int32_t>(nowMs - it->second) >= 0) {
      it = sIconRetryAfterMs.erase(it);
      continue;
    }
    ++it;
  }
  while (sIconRetryAfterMs.size() > kIconRetryMaxEntries) {
    auto victim = sIconRetryAfterMs.begin();
    for (auto it = sIconRetryAfterMs.begin(); it != sIconRetryAfterMs.end(); ++it) {
      if (it->second > victim->second) {
        victim = it;
      }
    }
    sIconRetryAfterMs.erase(victim);
  }
}

void setIconRetryAfter(const std::string& key, uint32_t nowMs, uint32_t delayMs) {
  pruneIconRetryMap(nowMs);
  sIconRetryAfterMs[key] = nowMs + delayMs;
}

bool reclaimRuntimeCachesLowHeap(const char* reason) {
  const uint32_t nowMs = platform::millisMs();
  if (nowMs - sLastLowHeapRecoverMs < kLowHeapRecoverCooldownMs) {
    return false;
  }
  sLastLowHeapRecoverMs = nowMs;

  const size_t freeBefore = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t largestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const size_t iconEntries = sIconMemCache.size();
  const size_t iconBytes = sIconMemCacheBytes;
  const size_t httpEntries = sSharedHttpCache.size();
  const size_t canvasBytes = sCanvasCapacityBytes;

  sIconMemCache.clear();
  sIconMemCacheBytes = 0;
  sIconRetryAfterMs.clear();
  std::map<std::string, IconMemEntry>().swap(sIconMemCache);
  std::map<std::string, uint32_t>().swap(sIconRetryAfterMs);
  sSharedHttpCache.clear();
  std::map<std::string, SharedHttpResponse>().swap(sSharedHttpCache);
  if (sCanvas != nullptr) {
    heap_caps_free(sCanvas);
    sCanvas = nullptr;
  }
  sCanvasCapacityBytes = 0;
  sCanvasW = 0;
  sCanvasH = 0;
  sCanvasY0 = 0;

  size_t wsCacheEntries = 0;
  if (takeWsLock(10)) {
    wsCacheEntries = sWs.cacheJsonByKey.size();
    sWs.cacheJsonByKey.clear();
    std::map<std::string, std::string>().swap(sWs.cacheJsonByKey);
    giveWsLock();
  }

  const size_t freeAfter = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t largestAfter = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  ESP_LOGW(kTag,
           "low-heap reclaim reason=%s icon_entries=%u icon_bytes=%u http_entries=%u ws_cache=%u canvas_bytes=%u free=%u->%u largest=%u->%u",
           reason != nullptr ? reason : "(unknown)", static_cast<unsigned>(iconEntries),
           static_cast<unsigned>(iconBytes), static_cast<unsigned>(httpEntries),
           static_cast<unsigned>(wsCacheEntries), static_cast<unsigned>(canvasBytes),
           static_cast<unsigned>(freeBefore), static_cast<unsigned>(freeAfter),
           static_cast<unsigned>(largestBefore), static_cast<unsigned>(largestAfter));
  return largestAfter > largestBefore || freeAfter > freeBefore;
}

void iconMemCachePut(const std::string& key, std::vector<uint16_t>&& pixels) {
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
  entry.pixels = std::move(pixels);
  entry.lastUsedMs = platform::millisMs();
  sIconMemCacheBytes += bytes;
  sIconMemCache[key] = std::move(entry);
}

bool iconFileCacheGet(const std::string& key, int w, int h, std::vector<uint16_t>& outPixels) {
  if (!ensureIconCacheDir()) {
    ESP_LOGI(kTag, "icon file cache miss key=%s reason=dir_unavailable", key.c_str());
    return false;
  }
  const size_t needPixels = static_cast<size_t>(w) * static_cast<size_t>(h);
  const size_t needBytes = needPixels * sizeof(uint16_t);
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (largest < needBytes + 1024U) {
    return false;
  }
  const std::string path = iconCacheFilePath(key);
  std::FILE* fp = std::fopen(path.c_str(), "rb");
  if (fp == nullptr) {
    ESP_LOGI(kTag, "icon file cache miss key=%s path=%s reason=no_file", key.c_str(), path.c_str());
    return false;
  }
  outPixels.resize(needPixels);
  const size_t got = std::fread(outPixels.data(), sizeof(uint16_t), needPixels, fp);
  std::fclose(fp);
  if (got == needPixels) {
    ESP_LOGI(kTag, "icon file cache hit key=%s path=%s bytes=%u", key.c_str(), path.c_str(),
             static_cast<unsigned>(needBytes));
    return true;
  }
  ESP_LOGW(kTag, "icon file cache miss key=%s path=%s reason=size_mismatch got=%u expect=%u",
           key.c_str(), path.c_str(), static_cast<unsigned>(got * sizeof(uint16_t)),
           static_cast<unsigned>(needBytes));
  return false;
}

void iconFileCachePut(const std::string& key, const std::vector<uint16_t>& pixels) {
  if (!ensureIconCacheDir()) {
    ESP_LOGW(kTag, "icon file cache skip key=%s reason=dir_unavailable", key.c_str());
    return;
  }
  const std::string path = iconCacheFilePath(key);
  const size_t bytes = pixels.size() * sizeof(uint16_t);
  if (!enforceIconFileCacheBudget(path, bytes)) {
    ESP_LOGW(kTag, "icon file cache skip key=%s reason=budget path=%s bytes=%u", key.c_str(),
             path.c_str(), static_cast<unsigned>(bytes));
    return;
  }
  std::FILE* fp = std::fopen(path.c_str(), "wb");
  if (fp == nullptr) {
    ESP_LOGW(kTag, "icon file cache skip key=%s reason=open_failed path=%s", key.c_str(), path.c_str());
    return;
  }
  (void)std::fwrite(pixels.data(), sizeof(uint16_t), pixels.size(), fp);
  std::fclose(fp);
  ESP_LOGI(kTag, "icon file cache store key=%s path=%s bytes=%u", key.c_str(), path.c_str(),
           static_cast<unsigned>(bytes));
}

bool loadIconRemote(const std::string& url, int w, int h, bool allowNetwork = true,
                    bool* outUsedNetwork = nullptr) {
  if (outUsedNetwork != nullptr) {
    *outUsedNetwork = false;
  }
  if (url.empty() || w <= 0 || h <= 0) {
    return false;
  }
  const std::string key = iconCacheKey(url, w, h);
  const uint32_t nowMs = platform::millisMs();
  pruneIconRetryMap(nowMs);
  const std::vector<uint16_t>* cachedPixels = nullptr;
  if (iconMemCacheGetPtr(key, cachedPixels) && cachedPixels != nullptr) {
    sIconRetryAfterMs.erase(key);
    return true;
  }
  std::vector<uint16_t> filePixels;
  if (iconFileCacheGet(key, w, h, filePixels)) {
    iconMemCachePut(key, std::move(filePixels));
    sIconRetryAfterMs.erase(key);
    return true;
  }
  auto retryIt = sIconRetryAfterMs.find(key);
  if (retryIt != sIconRetryAfterMs.end() && nowMs < retryIt->second) {
    return false;
  }
  if (!allowNetwork) {
    return false;
  }
  if (outUsedNetwork != nullptr) {
    *outUsedNetwork = true;
  }

  int status = 0;
  std::string body;
  std::string reason;
  // The HTTP client connection needs ~12KB contiguous (socket + internal
  // buffers). If the worker task doesn't exist yet, httpGet will create it
  // first, consuming kHttpWorkerStack (~8KB) from the largest block before the
  // HTTP client runs. Post-WS-TLS, largest is ~19KB — 19 - 8 = 11KB, not
  // enough margin. Require the full combined amount when no worker exists so
  // the guard fires and the icon is skipped rather than fragmenting the heap
  // permanently (which would block all subsequent WS fetch-guard checks).
  // Plain-HTTP icon server (no TLS): client needs ~4-6KB contiguous for socket
  // + internal buffers. Use 8KB as a safe floor with worker present.
  // Without the worker, httpGet creates it first (8KB stack), so require the
  // combined amount. Post-WS-TLS the worker should already be pre-created by
  // the auth_ok handler, so the no-worker branch is a fallback only.
  const size_t kIconFetchMinLargest =
      (sHttpWorkerTask != nullptr) ? 8U * 1024U : (8U * 1024U + kHttpWorkerStack);
  // Do NOT call reclaimRuntimeCachesLowHeap here — it clears the WS JSON
  // cache which contains live subscription data. Losing that cache means the
  // widget shows "Loading..." until the next WS event arrives, which is worse
  // than simply deferring the icon fetch.
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (largest < kIconFetchMinLargest) {
    setIconRetryAfter(key, nowMs, kIconFetchRetryMs);
    ESP_LOGW(kTag, "icon fetch skipped low_heap url=%s largest=%u", url.c_str(),
             static_cast<unsigned>(largest));
    return false;
  }
  if (!httpGet(url, {}, status, body, reason)) {
    ESP_LOGW(kTag, "icon fetch fail url=%s reason=%s", url.c_str(), reason.c_str());
    setIconRetryAfter(key, nowMs, kIconFetchRetryMs);
    return false;
  }
  if (status < 200 || status >= 300) {
    ESP_LOGW(kTag, "icon fetch status=%d url=%s", status, url.c_str());
    setIconRetryAfter(key, nowMs, kIconFetchRetryMs);
    return false;
  }
  const size_t needBytes = static_cast<size_t>(w) * static_cast<size_t>(h) * sizeof(uint16_t);
  if (body.size() != needBytes) {
    ESP_LOGW(kTag, "icon fetch size mismatch url=%s got=%u expect=%u", url.c_str(),
             static_cast<unsigned>(body.size()), static_cast<unsigned>(needBytes));
    setIconRetryAfter(key, nowMs, kIconFetchRetryMs);
    return false;
  }
  const size_t needPixels = static_cast<size_t>(w) * static_cast<size_t>(h);
  const size_t largestBeforePixels = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (largestBeforePixels < needBytes + 1024U) {
    setIconRetryAfter(key, nowMs, kIconFetchRetryMs);
    ESP_LOGW(kTag, "icon decode skipped low_heap url=%s largest=%u need=%u", url.c_str(),
             static_cast<unsigned>(largestBeforePixels), static_cast<unsigned>(needBytes));
    return false;
  }
  std::vector<uint16_t> pixels(needPixels);
  std::memcpy(pixels.data(), body.data(), needBytes);
  iconFileCachePut(key, pixels);
  iconMemCachePut(key, std::move(pixels));
  sIconRetryAfterMs.erase(key);
  return true;
}

void prefetchRemoteIconsForCurrentState() {
  for (const Node& node : s.nodes) {
    if (node.type != NodeType::kIcon || node.w <= 0 || node.h <= 0) {
      continue;
    }
    const std::string rawPath = node.path.empty() ? node.text : node.path;
    const std::string iconPath = bindRuntimeTemplate(rawPath);
    if (!isHttpUrl(iconPath)) {
      continue;
    }
    const std::string key = iconCacheKey(iconPath, node.w, node.h);
    const std::vector<uint16_t>* cachedPixels = nullptr;
    if (iconMemCacheGetPtr(key, cachedPixels) && cachedPixels != nullptr) {
      continue;
    }
    // Avoid allocator spikes by attempting at most one network icon fetch per pass.
    // Cache hits (memory or file-cache) should continue immediately.
    bool usedNetwork = false;
    (void)loadIconRemote(iconPath, node.w, node.h, true, &usedNetwork);
    if (usedNetwork) {
      break;
    }
  }
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

bool nodeVisible(const Node& node) {
  if (node.visibleIf.empty()) {
    return true;
  }
  const std::string expr = bindRuntimeTemplate(node.visibleIf);
  const std::string trimmed = trimCopy(expr);
  if (trimmed.empty()) {
    return true;
  }
  std::string lowered = trimmed;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (lowered == "true" || lowered == "yes" || lowered == "on") {
    return true;
  }
  if (lowered == "false" || lowered == "no" || lowered == "off") {
    return false;
  }
  float numeric = 0.0f;
  if (evalNumericExpr(trimmed, nullptr, numeric)) {
    return std::fabs(numeric) > 0.000001f;
  }
  double parsed = 0.0;
  if (parseStrictDouble(trimmed, parsed)) {
    return std::fabs(parsed) > 0.000001;
  }
  if (const std::string* value = getValue(trimmed); value != nullptr) {
    const std::string vtrim = trimCopy(*value);
    if (vtrim.empty()) {
      return false;
    }
    std::string vlow = vtrim;
    std::transform(vlow.begin(), vlow.end(), vlow.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    if (vlow == "false" || vlow == "no" || vlow == "off") {
      return false;
    }
    if (vlow == "true" || vlow == "yes" || vlow == "on") {
      return true;
    }
    double vnum = 0.0;
    if (parseStrictDouble(vtrim, vnum)) {
      return std::fabs(vnum) > 0.000001;
    }
    return true;
  }
  bool known = false;
  const std::string knownValue = resolveKnownToken(trimmed, &known);
  if (known) {
    const std::string ktrim = trimCopy(knownValue);
    if (ktrim.empty()) {
      return false;
    }
    double knum = 0.0;
    if (parseStrictDouble(ktrim, knum)) {
      return std::fabs(knum) > 0.000001;
    }
    std::string klow = ktrim;
    std::transform(klow.begin(), klow.end(), klow.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    if (klow == "false" || klow == "no" || klow == "off") {
      return false;
    }
    return true;
  }
  return false;
}

int resolveNodeCoord(const std::string& expr, int fallback) {
  if (expr.empty()) {
    return fallback;
  }
  const std::string bound = bindRuntimeTemplate(expr);
  const std::string trimmed = trimCopy(bound);
  if (trimmed.empty()) {
    return fallback;
  }
  float numeric = 0.0f;
  if (evalNumericExpr(trimmed, nullptr, numeric)) {
    return static_cast<int>(std::lround(numeric));
  }
  double parsed = 0.0;
  if (parseStrictDouble(trimmed, parsed)) {
    return static_cast<int>(std::lround(parsed));
  }
  return fallback;
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
    if (!nodeVisible(node)) {
      continue;
    }
    const int localX = resolveNodeCoord(node.xExpr, node.x);
    const int localY = resolveNodeCoord(node.yExpr, node.y);
    const int x = static_cast<int>(s.x) + localX;
    const int y = static_cast<int>(s.y) + localY;
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
      drawLineThick(x, y, x2, y2, thickness, node.color565);
      continue;
    }

    if (node.type == NodeType::kIcon) {
      const std::string rawPath = node.path.empty() ? node.text : node.path;
      const std::string iconPath = bindRuntimeTemplate(rawPath);
      if (iconPath.empty() || node.w <= 0 || node.h <= 0) {
        continue;
      }
      std::vector<uint16_t> pixels;
      const std::vector<uint16_t>* pixelsRef = nullptr;
      if (isHttpUrl(iconPath)) {
        const std::string cacheKey = iconCacheKey(iconPath, node.w, node.h);
        if (!iconMemCacheGetPtr(cacheKey, pixelsRef) || pixelsRef == nullptr) {
          continue;
        }
      } else {
        const bool ok = loadIcon(iconPath, node.w, node.h, pixels);
        if (!ok) {
          continue;
        }
        pixelsRef = &pixels;
      }
      if (canvasActive()) {
        for (int iy = 0; iy < node.h; ++iy) {
          for (int ix = 0; ix < node.w; ++ix) {
            drawPixel(x + ix, y + iy,
                      (*pixelsRef)[static_cast<size_t>(iy) * static_cast<size_t>(node.w) +
                                   static_cast<size_t>(ix)]);
          }
        }
      } else {
        (void)display_spi::drawRgb565(static_cast<uint16_t>(x), static_cast<uint16_t>(y),
                                      static_cast<uint16_t>(node.w), static_cast<uint16_t>(node.h),
                                      pixelsRef->data());
      }
      continue;
    }

    if (node.type == NodeType::kBitmap1) {
      const std::string rawPath = node.path.empty() ? node.text : node.path;
      const std::string bitmapPath = bindRuntimeTemplate(rawPath);
      if (bitmapPath.empty() || node.w <= 0 || node.h <= 0) {
        continue;
      }
      if (isHttpUrl(bitmapPath)) {
        // First pass: bitmap1 is local-file based to avoid repeated HTTP fetches.
        continue;
      }
      std::vector<uint8_t> bits;
      if (!loadBitmap1(bitmapPath, node.w, node.h, bits)) {
        continue;
      }
      const size_t stride = static_cast<size_t>((node.w + 7) / 8);
      for (int iy = 0; iy < node.h; ++iy) {
        const size_t rowBase = static_cast<size_t>(iy) * stride;
        for (int ix = 0; ix < node.w; ++ix) {
          const size_t byteIndex = rowBase + static_cast<size_t>(ix / 8);
          const uint8_t mask = static_cast<uint8_t>(0x80U >> (ix & 7));
          const bool on = (bits[byteIndex] & mask) != 0;
          drawPixel(x + ix, y + iy, on ? node.color565 : node.bg565);
        }
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
      // Normalize phase to [0, 1): 0=new, 0.25=first quarter, 0.5=full, 0.75=last quarter.
      phase = std::fmod(phase, 1.0f);
      if (phase < 0.0f) {
        phase += 1.0f;
      }
      const int r = node.radius > 0 ? node.radius : (node.w > 0 ? node.w / 2 : 8);
      if (r <= 0) {
        continue;
      }
      drawCircle(x, y, r, node.bg565, true);
      // Lit-side model using a moving vertical terminator:
      // terminatorX(y) = cos(2*pi*phase) * sqrt(r^2 - y^2)
      // Waxing lights the right side; waning lights the left side.
      const bool waxing = phase <= 0.5f;
      const float cosine = std::cos(2.0f * static_cast<float>(M_PI) * phase);
      for (int dy = -r; dy <= r; ++dy) {
        const float chord = std::sqrt(static_cast<float>(r * r - dy * dy));
        const float terminator = cosine * chord;
        for (int dx = -r; dx <= r; ++dx) {
          if (dx * dx + dy * dy > r * r) {
            continue;
          }
          const bool lit = waxing ? (static_cast<float>(dx) > terminator)
                                  : (static_cast<float>(dx) < -terminator);
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
  // Don't allocate the canvas during the initial LOADING render — defer until
  // we have real data. This keeps the large contiguous block available for the
  // first TLS handshake, which needs at least 20KB contiguous.
  if (!s.hasData && sCanvas == nullptr) {
    drawSolidRect(s.x, s.y, s.w, s.h, kBg);
    if (s.drawBorder) {
      drawSolidRect(s.x, s.y, s.w, 1, kBorder);
      drawSolidRect(s.x, static_cast<int>(s.y + s.h - 1), s.w, 1, kBorder);
      drawSolidRect(s.x, s.y, 1, s.h, kBorder);
      drawSolidRect(static_cast<int>(s.x + s.w - 1), s.y, 1, s.h, kBorder);
    }
    drawText(static_cast<int>(s.x) + 6, static_cast<int>(s.y) + 22, "LOADING...", kText, kBg, 1);
    return;
  }

  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const size_t targetBytes = (largest * 3U) / 4U;
  uint16_t desiredRows =
      static_cast<uint16_t>(std::clamp<size_t>(targetBytes / bytesPerRow, 1U, static_cast<size_t>(s.h)));
  size_t desiredBytes = bytesPerRow * static_cast<size_t>(desiredRows);
  const size_t fullWidgetBytes = bytesPerRow * static_cast<size_t>(s.h);
  const size_t cappedBytes = std::max<size_t>(
      bytesPerRow, std::min<size_t>(kCanvasPersistentMaxBytes, fullWidgetBytes));
  if (desiredBytes > cappedBytes) {
    desiredBytes = cappedBytes;
    desiredRows = static_cast<uint16_t>(std::max<size_t>(1U, desiredBytes / bytesPerRow));
  }

  if (sCanvas == nullptr || sCanvasCapacityBytes < desiredBytes) {
    uint16_t tryRows = desiredRows;
    size_t tryBytes = desiredBytes;
    uint16_t* newCanvas = nullptr;
    while (newCanvas == nullptr && tryRows > 0) {
      tryBytes = bytesPerRow * static_cast<size_t>(tryRows);
      newCanvas = static_cast<uint16_t*>(heap_caps_malloc(tryBytes, MALLOC_CAP_8BIT));
      if (newCanvas == nullptr && tryRows > 1) {
        tryRows = static_cast<uint16_t>(tryRows / 2U);
      } else {
        break;
      }
    }
    if (newCanvas != nullptr) {
      if (sCanvas != nullptr) {
        heap_caps_free(sCanvas);
      }
      sCanvas = newCanvas;
      sCanvasCapacityBytes = tryBytes;
    }
  }

  if (sCanvas == nullptr || sCanvasCapacityBytes < bytesPerRow) {
    ESP_LOGW(kTag, "widget=%s canvas alloc failed largest=%u row_bytes=%u; using direct draw",
             s.widgetId.c_str(), static_cast<unsigned>(largest), static_cast<unsigned>(bytesPerRow));
    drawSolidRect(s.x, s.y, s.w, s.h, kBg);
    if (s.drawBorder) {
      drawSolidRect(s.x, s.y, s.w, 1, kBorder);
      drawSolidRect(s.x, static_cast<int>(s.y + s.h - 1), s.w, 1, kBorder);
      drawSolidRect(s.x, s.y, 1, s.h, kBorder);
      drawSolidRect(static_cast<int>(s.x + s.w - 1), s.y, 1, s.h, kBorder);
    }
    drawText(static_cast<int>(s.x) + 4, static_cast<int>(s.y) + 4, s.widgetId, kAccent, kBg, 1);
    if (!s.hasData) {
      drawText(static_cast<int>(s.x) + 6, static_cast<int>(s.y) + 22, "LOADING...", kText, kBg, 1);
      return;
    }
    renderNodes();
    renderActiveModal();
    return;
  }

  uint16_t bandRows =
      static_cast<uint16_t>(std::max<size_t>(1U, sCanvasCapacityBytes / bytesPerRow));
  bandRows = std::min<uint16_t>(bandRows, s.h);
  sCanvasW = s.w;
  for (uint16_t row = 0; row < s.h; row = static_cast<uint16_t>(row + bandRows)) {
    const uint16_t rowsThis = std::min<uint16_t>(bandRows, static_cast<uint16_t>(s.h - row));
    sCanvasH = rowsThis;
    sCanvasY0 = static_cast<uint16_t>(s.y + row);
    std::fill(sCanvas, sCanvas + static_cast<size_t>(sCanvasW) * static_cast<size_t>(sCanvasH), kBg);

    if (s.drawBorder) {
      drawSolidRect(s.x, s.y, s.w, 1, kBorder);
      drawSolidRect(s.x, static_cast<int>(s.y + s.h - 1), s.w, 1, kBorder);
      drawSolidRect(s.x, s.y, 1, s.h, kBorder);
      drawSolidRect(static_cast<int>(s.x + s.w - 1), s.y, 1, s.h, kBorder);
    }
    if (s.showTitle && !s.uiTitle.empty()) {
      drawText(static_cast<int>(s.x) + 4, static_cast<int>(s.y) + 4, bindRuntimeTemplate(s.uiTitle),
               kAccent, kBg, 1);
    }

    if (!s.hasData) {
      drawText(static_cast<int>(s.x) + 6, static_cast<int>(s.y) + 22, "LOADING...", kText, kBg, 1);
    } else {
      renderNodes();
      renderActiveModal();
    }

    (void)display_spi::drawRgb565(s.x, sCanvasY0, sCanvasW, sCanvasH, sCanvas);
  }
  sCanvasW = 0;
  sCanvasH = 0;
  sCanvasY0 = 0;
}

}  // namespace

namespace dsl_widget_runtime {

void reset() {
  const size_t iconEntries = sIconMemCache.size();
  const size_t iconBytes = sIconMemCacheBytes;
  const size_t httpReuseEntries = sSharedHttpCache.size();
  const size_t instances = sInstances.size();
  const size_t canvasBytes = sCanvasCapacityBytes;
  const size_t freeBefore = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t largestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  teardownHttpWorker();
  sIconMemCache.clear();
  sIconMemCacheBytes = 0;
  sIconRetryAfterMs.clear();
  sLastLowHeapRecoverMs = 0;
  sSharedHttpCache.clear();
  sHttpStartupOrdinal = 0;
  if (sCanvas != nullptr) {
    heap_caps_free(sCanvas);
    sCanvas = nullptr;
  }
  sCanvasCapacityBytes = 0;
  sCanvasW = 0;
  sCanvasH = 0;
  sCanvasY0 = 0;
  teardownWsClient();
  if (sWs.lock != nullptr) {
    vSemaphoreDelete(sWs.lock);
    sWs.lock = nullptr;
  }
  settleAfterNetworkTeardown();
  s = {};
  sInstances.clear();
  std::vector<State>().swap(sInstances);
  sSharedLookupsJsonCache.clear();
  std::map<std::string, std::map<std::string, std::string>>().swap(sSharedFormatLookups);
  std::map<std::string, IconMemEntry>().swap(sIconMemCache);
  std::map<std::string, uint32_t>().swap(sIconRetryAfterMs);
  std::map<std::string, SharedHttpResponse>().swap(sSharedHttpCache);
  const size_t freeAfter = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t largestAfter = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (iconEntries > 0 || iconBytes > 0 || httpReuseEntries > 0 || canvasBytes > 0 || instances > 0) {
    ESP_LOGI(kTag,
             "reset cleared ws+http+icons+canvas+instances instances=%u icon_entries=%u icon_bytes=%u "
             "http_reuse=%u canvas_bytes=%u free=%u->%u largest=%u->%u",
             static_cast<unsigned>(instances), static_cast<unsigned>(iconEntries), static_cast<unsigned>(iconBytes),
             static_cast<unsigned>(httpReuseEntries), static_cast<unsigned>(canvasBytes),
             static_cast<unsigned>(freeBefore), static_cast<unsigned>(freeAfter),
             static_cast<unsigned>(largestBefore), static_cast<unsigned>(largestAfter));
  } else {
    ESP_LOGI(kTag, "reset complete free=%u->%u largest=%u->%u", static_cast<unsigned>(freeBefore),
             static_cast<unsigned>(freeAfter), static_cast<unsigned>(largestBefore),
             static_cast<unsigned>(largestAfter));
  }
}

bool begin(const char* widgetId, const char* dslPath, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
           const char* settingsJson, const char* sharedSettingsJson, const char* sharedLookupsJson,
           bool drawBorder) {
  State previous = std::move(s);
  s = {};
  s.active = true;
  s.widgetId = (widgetId != nullptr) ? widgetId : "dsl";
  s.dslPath = (dslPath != nullptr) ? dslPath : "";
  s.x = x;
  s.y = y;
  s.w = w;
  s.h = h;
  s.drawBorder = drawBorder;
  loadWidgetSettings(settingsJson, sharedSettingsJson);
  refreshSharedLookupsCache(sharedLookupsJson);

  const std::string dslJson = readFile(s.dslPath.c_str());
  if (dslJson.empty() || !loadDslConfig(dslJson)) {
    ESP_LOGE(kTag, "dsl load failed widget=%s path=%s", s.widgetId.c_str(), s.dslPath.c_str());
    s.active = false;
    s = std::move(previous);
    return false;
  }
  loadTapActionFromSettings();

  ESP_LOGI(kTag,
           "begin widget=%s path=%s source=%d poll_ms=%u fields=%u nodes=%u modals=%u touch_regions=%u settings=%u "
           "http_max=%u tls_skip_verify=%d retain_source=%d",
           s.widgetId.c_str(), s.dslPath.c_str(), static_cast<int>(s.source),
           static_cast<unsigned>(s.pollMs), static_cast<unsigned>(s.fields.size()),
           static_cast<unsigned>(s.nodes.size()), static_cast<unsigned>(s.modals.size()),
           static_cast<unsigned>(s.touchRegions.size()), static_cast<unsigned>(s.settingValues.size()),
           static_cast<unsigned>(s.httpMaxBytes), s.tlsSkipVerify ? 1 : 0, s.retainSourceJson ? 1 : 0);

  // Pre-create the HTTP worker task when the first HTTP-source widget is
  // registered so its stack is carved from the large contiguous heap block at
  // init time, before any TLS allocations fragment it.
  // Do NOT pre-create for WebSocket widgets with remote icons: those connect
  // WS+TLS first (~40KB), and the HTTP worker must be created lazily afterward
  // from the ~20KB remaining — pre-creating it beforehand steals heap the WS
  // TLS handshake needs, pushing largest below the recovery threshold.
  if (s.source == DataSource::kHttp) {
    const uint32_t nowMs = platform::millisMs();
    const uint32_t delayMs = std::min<uint32_t>(sHttpStartupOrdinal * kHttpStartupStaggerMs,
                                                kHttpStartupStaggerMaxMs);
    if (delayMs > 0) {
      s.backoffUntilMs = nowMs + delayMs;
      ESP_LOGI(kTag, "startup stagger widget=%s delay_ms=%u", s.widgetId.c_str(),
               static_cast<unsigned>(delayMs));
    }
    ++sHttpStartupOrdinal;
    (void)ensureHttpWorker();
  }

  render();
  sInstances.push_back(std::move(s));
  s = std::move(previous);
  return true;
}

bool tick(uint32_t nowMs) {
  bool drew = false;
  bool triggerWsBootstrap = false;
  if (takeWsLock(20)) {
    if (sWs.bootstrapTriggerPending) {
      triggerWsBootstrap = true;
      sWs.bootstrapTriggerPending = false;
    }
    for (auto it = sWs.pendingReqById.begin(); it != sWs.pendingReqById.end();) {
      if (nowMs - it->second.sentAtMs > kWsPendingReqTimeoutMs) {
        sWs.cacheKeyToPendingBootstrapReq.erase(it->second.cacheKey);
        sWs.cacheKeyToPendingSubscribeReq.erase(it->second.cacheKey);
        it = sWs.pendingReqById.erase(it);
      } else {
        ++it;
      }
    }
    giveWsLock();
  }
  if (triggerWsBootstrap) {
    int triggered = 0;
    for (State& inst : sInstances) {
      if (!inst.active || inst.source != DataSource::kWebSocket) {
        continue;
      }
      inst.lastFetchMs = 0;
      inst.backoffUntilMs = 0;
      ++triggered;
    }
    ESP_LOGI(kTag, "ws refresh trigger widgets=%d", triggered);
  }
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
        render();
        drew = true;
      }
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
        std::string url = bindRuntimeTemplate(tr.httpUrl);
        std::string body = bindRuntimeTemplate(tr.httpBody);
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
        const bool ok = httpRequest(std::move(method), std::move(url), std::move(headers),
                                    std::move(body), status, resp, reason);
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
      if (tr.action == TouchActionType::kWsPublish) {
        const std::string url = bindRuntimeTemplate(tr.wsUrl);
        std::string reason;
        std::string domain;
        std::string service;
        if (!url.empty() && parseHaServiceFromUrl(url, domain, service)) {
          const std::string body = bindRuntimeTemplate(tr.wsBody);
          const bool ok = wsCallService(domain, service, body, reason);
          if (!ok) {
            ESP_LOGW(kTag, "tap touch_region ws fail widget=%s reason=%s",
                     s.widgetId.c_str(), reason.c_str());
            instance = std::move(s);
            s = {};
            return false;
          }
        } else {
          const std::string body = bindRuntimeTemplatePreserveUnknown(tr.wsBody);
          ESP_LOGI(kTag, "tap touch_region ws publish widget=%s body=%s", s.widgetId.c_str(), body.c_str());
          const bool ok = wsPublishMessage(body, reason);
          if (!ok) {
            ESP_LOGW(kTag, "tap touch_region ws fail widget=%s reason=%s",
                     s.widgetId.c_str(), reason.c_str());
            instance = std::move(s);
            s = {};
            return false;
          }
        }
        ESP_LOGI(kTag, "tap touch_region ws ok widget=%s", s.widgetId.c_str());
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
