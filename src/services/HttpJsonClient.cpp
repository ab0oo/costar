#include "services/HttpJsonClient.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <cstdlib>
#include <esp_heap_caps.h>

namespace {
String readPayloadFromStream(HTTPClient& http) {
  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    return String();
  }

  String payload;
  const int sizeHint = http.getSize();
  if (sizeHint > 0) {
    payload.reserve(sizeHint);
  }

  const uint32_t timeoutMs = 8000;
  const uint32_t startMs = millis();
  uint32_t lastByteMs = startMs;

  while ((http.connected() || stream->available() > 0) && (millis() - startMs) < timeoutMs) {
    const size_t availableBytes = stream->available();
    if (availableBytes > 0) {
      char buf[128];
      const size_t toRead = availableBytes > sizeof(buf) ? sizeof(buf) : availableBytes;
      const int readCount = stream->readBytes(buf, toRead);
      if (readCount > 0) {
        payload.concat(buf, readCount);
        lastByteMs = millis();
      }
      continue;
    }

    if ((millis() - lastByteMs) > 600) {
      break;
    }
    delay(5);
  }

  return payload;
}

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
}  // namespace

bool HttpJsonClient::get(const String& url, JsonDocument& outDoc,
                         String* errorMessage, HttpFetchMeta* meta) const {
  if (meta != nullptr) {
    *meta = HttpFetchMeta();
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (errorMessage != nullptr) {
      *errorMessage = "WiFi disconnected, " + heapDiag();
    }
    return false;
  }

  HTTPClient http;
  WiFiClientSecure secureClient;
  int statusCode = 0;
  if (url.startsWith("https://")) {
    secureClient.setInsecure();
    if (!http.begin(secureClient, url)) {
      if (errorMessage != nullptr) {
        *errorMessage = "HTTP begin failed";
      }
      return false;
    }
  } else {
    if (!http.begin(url)) {
      if (errorMessage != nullptr) {
        *errorMessage = "HTTP begin failed";
      }
      return false;
    }
  }
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setRedirectLimit(5);
  http.setConnectTimeout(4500);
  http.setTimeout(6500);
  http.useHTTP10(true);
  http.setReuse(false);
  const char* headerKeys[] = {"Content-Type", "Content-Length", "Transfer-Encoding",
                              "Location", "Content-Encoding"};
  http.collectHeaders(headerKeys, 5);
  http.addHeader("Accept", "application/json");
  http.addHeader("User-Agent", "CoStar-ESP32/1.0");
  http.addHeader("Accept-Encoding", "identity");
  statusCode = http.GET();

  if (meta != nullptr) {
    meta->statusCode = statusCode;
  }

  if (statusCode <= 0) {
    if (errorMessage != nullptr) {
      *errorMessage = "HTTP GET failed code=" + String(statusCode) + " reason='" +
                      http.errorToString(statusCode) + "', " + heapDiag();
    }
    http.end();
    return false;
  }

  if (statusCode < 200 || statusCode >= 300) {
    if (errorMessage != nullptr) {
      *errorMessage = "HTTP status " + String(statusCode) + ", location='" +
                      http.header("Location") + "', " + heapDiag();
    }
    http.end();
    return false;
  }

  const String contentType = http.header("Content-Type");
  const String contentLengthHeader = http.header("Content-Length");
  const String transferEncodingHeader = http.header("Transfer-Encoding");
  const String contentEncodingHeader = http.header("Content-Encoding");
  int contentLengthBytes = -1;
  if (!contentLengthHeader.isEmpty()) {
    char* endPtr = nullptr;
    const long parsed = strtol(contentLengthHeader.c_str(), &endPtr, 10);
    if (endPtr != nullptr && *endPtr == '\0' && parsed >= 0) {
      contentLengthBytes = static_cast<int>(parsed);
    }
  }
  String payload = http.getString();
  if (payload.length() == 0) {
    payload = readPayloadFromStream(http);
  }
  http.end();

  if (meta != nullptr) {
    meta->contentType = contentType;
    meta->contentLengthBytes = contentLengthBytes;
    meta->payloadBytes = payload.length();
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
