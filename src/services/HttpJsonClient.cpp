#include "services/HttpJsonClient.h"
#include "services/HttpTransportGate.h"

#include <WiFi.h>
#include <cstdlib>
#include <esp_crt_bundle.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>

namespace {
constexpr uint32_t kMinLargestBlockForTls = 14000U;
constexpr uint8_t kTransportFailureRecoveryThreshold = 6U;
constexpr uint32_t kRecoveryAttemptCooldownMs = 15000U;
constexpr uint32_t kTransportOutageCooldownMs = 12000U;
constexpr uint8_t kTransportOutageThreshold = 6U;

String heapDiag() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t minFree = ESP.getMinFreeHeap();
  const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  return "heap_free=" + String(freeHeap) + ", heap_min=" + String(minFree) +
         ", heap_largest=" + String(largest);
}

String extractLikelyJson(const String& payload) {
  int startObj = payload.indexOf('{');
  int startArr = payload.indexOf('[');

  int start = -1;
  if (startObj >= 0 && startArr >= 0) {
    start = (startObj < startArr) ? startObj : startArr;
  } else if (startObj >= 0) {
    start = startObj;
  } else if (startArr >= 0) {
    start = startArr;
  }

  if (start < 0) {
    return payload;
  }

  int endObj = payload.lastIndexOf('}');
  int endArr = payload.lastIndexOf(']');
  int end = (endObj > endArr) ? endObj : endArr;
  if (end < start) {
    return payload.substring(start);
  }

  return payload.substring(start, end + 1);
}

String compactPreview(const String& payload, size_t maxLen = 120) {
  String out = payload;
  out.replace('\n', ' ');
  out.replace('\r', ' ');
  out.trim();
  if (out.length() > static_cast<int>(maxLen)) {
    out = out.substring(0, maxLen) + "...";
  }
  return out;
}

struct HttpCapture {
  String body;
  String contentType;
  String contentLength;
  String transferEncoding;
  String contentEncoding;
  String location;
  String retryAfter;
};

esp_err_t httpEventHandler(esp_http_client_event_t* evt) {
  if (evt == nullptr || evt->user_data == nullptr) {
    return ESP_OK;
  }
  HttpCapture* cap = static_cast<HttpCapture*>(evt->user_data);
  switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER: {
      if (evt->header_key == nullptr || evt->header_value == nullptr) {
        break;
      }
      String key(evt->header_key);
      key.toLowerCase();
      const String value(evt->header_value);
      if (key == "content-type") {
        cap->contentType = value;
      } else if (key == "content-length") {
        cap->contentLength = value;
      } else if (key == "transfer-encoding") {
        cap->transferEncoding = value;
      } else if (key == "content-encoding") {
        cap->contentEncoding = value;
      } else if (key == "location") {
        cap->location = value;
      } else if (key == "retry-after") {
        cap->retryAfter = value;
      }
      break;
    }
    case HTTP_EVENT_ON_DATA: {
      if (evt->data != nullptr && evt->data_len > 0) {
        cap->body.concat(static_cast<const char*>(evt->data), evt->data_len);
      }
      break;
    }
    default:
      break;
  }
  return ESP_OK;
}

uint8_t sTransportFailureStreak = 0;
uint32_t sLastRecoveryAttemptMs = 0;
uint32_t sTransportOutageUntilMs = 0;

void resetTransportFailureStreak() { sTransportFailureStreak = 0; }

void noteTransportFailureAndMaybeRecover() {
  if (sTransportFailureStreak < 255) {
    ++sTransportFailureStreak;
  }
  if (sTransportFailureStreak >= kTransportOutageThreshold) {
    const uint32_t nowMs = millis();
    const uint32_t nextUntil = nowMs + kTransportOutageCooldownMs;
    if (static_cast<int32_t>(nextUntil - sTransportOutageUntilMs) > 0) {
      sTransportOutageUntilMs = nextUntil;
    }
  }
  if (sTransportFailureStreak < kTransportFailureRecoveryThreshold) {
    return;
  }

  const uint32_t nowMs = millis();
  if (static_cast<int32_t>(nowMs - sLastRecoveryAttemptMs) < static_cast<int32_t>(kRecoveryAttemptCooldownMs)) {
    return;
  }

  sLastRecoveryAttemptMs = nowMs;
  Serial.printf("[http] transport failure streak=%u, forcing WiFi reconnect\n",
                static_cast<unsigned>(sTransportFailureStreak));
  WiFi.disconnect(false, false);
  delay(60);
  WiFi.reconnect();
}

bool inTransportOutageCooldown(String* errorMessage, HttpFetchMeta* meta, uint32_t startMs) {
  if (sTransportOutageUntilMs == 0) {
    return false;
  }
  const uint32_t nowMs = millis();
  if (static_cast<int32_t>(nowMs - sTransportOutageUntilMs) >= 0) {
    sTransportOutageUntilMs = 0;
    return false;
  }
  const uint32_t remainingMs = sTransportOutageUntilMs - nowMs;
  if (meta != nullptr) {
    meta->statusCode = -3;
    meta->transportReason = "transport-cooldown";
    meta->elapsedMs = millis() - startMs;
  }
  if (errorMessage != nullptr) {
    *errorMessage = "Transport cooldown active (" + String(remainingMs) + " ms remaining), " +
                    heapDiag();
  }
  return true;
}

void clearTransportOutageCooldown() {
  sTransportOutageUntilMs = 0;
}

void clearTransportFailureState() {
  resetTransportFailureStreak();
  clearTransportOutageCooldown();
}

void noteSuccessfulHttpResponse() {
  clearTransportFailureState();
}

void noteTransportFailureAndReason(const String& transportReason) {
  noteTransportFailureAndMaybeRecover();
  if (transportReason.length() > 0) {
    Serial.printf("[http] transport fail streak=%u reason='%s'\n",
                  static_cast<unsigned>(sTransportFailureStreak), transportReason.c_str());
  }
}

void noteBeginFailureAndReason(const char* reason) {
  noteTransportFailureAndMaybeRecover();
  if (reason != nullptr && reason[0] != '\0') {
    Serial.printf("[http] begin fail streak=%u reason='%s'\n",
                  static_cast<unsigned>(sTransportFailureStreak), reason);
  }
}

}  // namespace

bool HttpJsonClient::get(const String& url, JsonDocument& outDoc,
                         String* errorMessage, HttpFetchMeta* meta,
                         const std::map<String, String>* extraHeaders) const {
  if (meta != nullptr) {
    *meta = HttpFetchMeta();
  }
  const uint32_t startMs = millis();
  if (inTransportOutageCooldown(errorMessage, meta, startMs)) {
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (errorMessage != nullptr) {
      *errorMessage = "WiFi disconnected, " + heapDiag();
    }
    return false;
  }

  if (url.startsWith("https://")) {
    const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largest < kMinLargestBlockForTls) {
      if (meta != nullptr) {
        meta->statusCode = -2;
        meta->transportReason = "tls-preflight-low-largest-block";
        meta->elapsedMs = millis() - startMs;
      }
      if (errorMessage != nullptr) {
        *errorMessage = "TLS preflight blocked: largest block too small (" + String(largest) +
                        " < " + String(kMinLargestBlockForTls) + "), " + heapDiag();
      }
      return false;
    }
  }

  httpgate::Guard guard(7000);
  if (!guard.locked()) {
    if (errorMessage != nullptr) {
      *errorMessage = "HTTP busy (transport gate timeout), " + heapDiag();
    }
    return false;
  }

  HttpCapture cap;
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.timeout_ms = 3500;
  cfg.disable_auto_redirect = false;
  cfg.max_redirection_count = 5;
  cfg.event_handler = httpEventHandler;
  cfg.user_data = &cap;
  cfg.buffer_size = 1024;
  cfg.buffer_size_tx = 512;
  cfg.skip_cert_common_name_check = false;
  cfg.crt_bundle_attach = arduino_esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) {
    noteBeginFailureAndReason("esp_http_client_init failed");
    if (errorMessage != nullptr) {
      *errorMessage = "HTTP init failed";
    }
    return false;
  }

  esp_http_client_set_method(client, HTTP_METHOD_GET);
  esp_http_client_set_header(client, "Accept", "application/json");
  esp_http_client_set_header(client, "User-Agent", "CoStar-ESP32/1.0");
  esp_http_client_set_header(client, "Accept-Encoding", "identity");
  if (extraHeaders != nullptr) {
    for (const auto& kv : *extraHeaders) {
      String name = kv.first;
      name.trim();
      if (name.isEmpty()) {
        continue;
      }
      if (name.indexOf('\r') >= 0 || name.indexOf('\n') >= 0) {
        continue;
      }
      String value = kv.second;
      value.replace("\r", "");
      value.replace("\n", "");
      if (value.isEmpty()) {
        continue;
      }
      esp_http_client_set_header(client, name.c_str(), value.c_str());
    }
  }

  const esp_err_t performErr = esp_http_client_perform(client);
  const int statusCode = (performErr == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
  const int contentLengthBytes = esp_http_client_get_content_length(client);

  if (meta != nullptr) {
    meta->statusCode = statusCode;
    meta->elapsedMs = millis() - startMs;
  }

  if (performErr != ESP_OK || statusCode <= 0) {
    String transportReason;
    if (performErr == ESP_OK) {
      transportReason = "no-http-status";
    } else {
      transportReason = String(esp_err_to_name(performErr));
      if (transportReason == "UNKNOWN ERROR") {
        transportReason += " (0x" + String(static_cast<unsigned long>(performErr), HEX) + ")";
      }
    }
    noteTransportFailureAndReason(transportReason);
    if (meta != nullptr) {
      meta->transportReason = transportReason;
    }
    if (errorMessage != nullptr) {
      *errorMessage = "HTTP transport failure (no HTTP status code) code=" +
                      String(statusCode) + " reason='" + transportReason +
                      "' (may fail before request reaches server), " + heapDiag();
    }
    esp_http_client_cleanup(client);
    return false;
  }

  noteSuccessfulHttpResponse();

  const String contentType = cap.contentType;
  const String contentLengthHeader = cap.contentLength;
  const String transferEncodingHeader = cap.transferEncoding;
  const String contentEncodingHeader = cap.contentEncoding;
  const String retryAfter = cap.retryAfter;
  if (contentLengthBytes < 0 && !contentLengthHeader.isEmpty()) {
    char* endPtr = nullptr;
    const long parsed = strtol(contentLengthHeader.c_str(), &endPtr, 10);
    if (endPtr != nullptr && *endPtr == '\0' && parsed >= 0) {
      // keep parsed header value only if IDF content-length unavailable
      (void)parsed;
    }
  }

  if (statusCode < 200 || statusCode >= 300) {
    String errorPayload = cap.body;
    if (meta != nullptr) {
      meta->contentType = contentType;
      meta->contentLengthBytes = contentLengthBytes;
      meta->payloadBytes = errorPayload.length();
      meta->retryAfter = retryAfter;
      meta->elapsedMs = millis() - startMs;
    }
    if (errorMessage != nullptr) {
      *errorMessage = "HTTP status " + String(statusCode) + ", location='" +
                      cap.location + "', retry-after='" + retryAfter +
                      "', preview='" + compactPreview(errorPayload, 120) + "', " +
                      heapDiag();
    }
    esp_http_client_cleanup(client);
    return false;
  }

  String payload = cap.body;
  esp_http_client_cleanup(client);

  if (meta != nullptr) {
    meta->contentType = contentType;
    meta->contentLengthBytes = contentLengthBytes;
    meta->payloadBytes = payload.length();
    meta->retryAfter = retryAfter;
    meta->elapsedMs = millis() - startMs;
  }

  if (payload.length() == 0) {
    if (errorMessage != nullptr) {
      *errorMessage = "Empty payload (status=" + String(statusCode) +
                      ", content-type='" + contentType + "', content-length='" +
                      contentLengthHeader + "', transfer-encoding='" +
                      transferEncodingHeader + "', content-encoding='" +
                      contentEncodingHeader + "'), " + heapDiag();
    }
    return false;
  }

  payload.trim();
  if (payload.startsWith("\xEF\xBB\xBF")) {
    payload = payload.substring(3);
  }

  String jsonBody = extractLikelyJson(payload);
  const DeserializationError err = deserializeJson(outDoc, jsonBody);
  if (err) {
    if (errorMessage != nullptr) {
      *errorMessage = "JSON parse failed (" + String(err.c_str()) +
                      "), bytes=" + String(payload.length()) + ", preview='" +
                      compactPreview(payload) + "', " + heapDiag();
    }
    return false;
  }

  return true;
}
