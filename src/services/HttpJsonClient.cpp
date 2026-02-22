#include "services/HttpJsonClient.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdlib>
#include <esp_heap_caps.h>

namespace {
struct ParsedUrl {
  String scheme;
  String host;
  uint16_t port = 0;
  String path;
};

bool parseUrl(const String& url, ParsedUrl& out) {
  const int schemeSep = url.indexOf("://");
  if (schemeSep <= 0) {
    return false;
  }
  out.scheme = url.substring(0, schemeSep);
  const int hostStart = schemeSep + 3;
  int pathStart = url.indexOf('/', hostStart);
  if (pathStart < 0) {
    pathStart = url.length();
    out.path = "/";
  } else {
    out.path = url.substring(pathStart);
  }
  if (out.path.isEmpty()) {
    out.path = "/";
  }

  const String hostPort = url.substring(hostStart, pathStart);
  if (hostPort.isEmpty()) {
    return false;
  }
  const int colon = hostPort.lastIndexOf(':');
  if (colon > 0) {
    const String maybePort = hostPort.substring(colon + 1);
    bool numeric = !maybePort.isEmpty();
    for (size_t i = 0; i < maybePort.length(); ++i) {
      if (maybePort[i] < '0' || maybePort[i] > '9') {
        numeric = false;
        break;
      }
    }
    if (numeric) {
      out.host = hostPort.substring(0, colon);
      out.port = static_cast<uint16_t>(maybePort.toInt());
    } else {
      out.host = hostPort;
    }
  } else {
    out.host = hostPort;
  }

  if (out.port == 0) {
    out.port = out.scheme == "https" ? 443 : 80;
  }
  return !out.host.isEmpty();
}

bool manualHttpsGetJson(const String& url, const std::map<String, String>* extraHeaders,
                        String& responseBody, int& statusCode, String& contentType,
                        int& contentLength) {
  ParsedUrl parsed;
  if (!parseUrl(url, parsed) || parsed.scheme != "https") {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(9000);
  if (!client.connect(parsed.host.c_str(), parsed.port, 2000)) {
    return false;
  }

  String req = "GET " + parsed.path + " HTTP/1.1\r\n";
  req += "Host: " + parsed.host + "\r\n";
  req += "Accept: application/json\r\n";
  req += "User-Agent: CoStar-ESP32/1.0\r\n";
  req += "Accept-Encoding: identity\r\n";
  req += "Connection: close\r\n";
  if (extraHeaders != nullptr) {
    for (const auto& kv : *extraHeaders) {
      String name = kv.first;
      name.trim();
      if (name.isEmpty() || name.indexOf('\r') >= 0 || name.indexOf('\n') >= 0) {
        continue;
      }
      String value = kv.second;
      value.replace("\r", "");
      value.replace("\n", "");
      if (value.isEmpty()) {
        continue;
      }
      req += name + ": " + value + "\r\n";
    }
  }
  req += "\r\n";
  client.print(req);

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  if (!statusLine.startsWith("HTTP/")) {
    client.stop();
    return false;
  }
  const int firstSpace = statusLine.indexOf(' ');
  if (firstSpace < 0) {
    client.stop();
    return false;
  }
  const int secondSpace = statusLine.indexOf(' ', firstSpace + 1);
  const String codeText =
      (secondSpace > firstSpace) ? statusLine.substring(firstSpace + 1, secondSpace)
                                 : statusLine.substring(firstSpace + 1);
  statusCode = codeText.toInt();

  contentType = "";
  contentLength = -1;
  for (;;) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) {
      break;
    }
    const int sep = line.indexOf(':');
    if (sep <= 0) {
      continue;
    }
    String key = line.substring(0, sep);
    String value = line.substring(sep + 1);
    key.trim();
    value.trim();
    key.toLowerCase();
    if (key == "content-type") {
      contentType = value;
    } else if (key == "content-length") {
      contentLength = value.toInt();
    }
  }

  responseBody = "";
  const uint32_t startMs = millis();
  uint32_t lastByteMs = startMs;
  while ((client.connected() || client.available() > 0) && (millis() - startMs) < 12000) {
    while (client.available() > 0) {
      const int ch = client.read();
      if (ch < 0) {
        break;
      }
      responseBody += static_cast<char>(ch);
      lastByteMs = millis();
    }
    if ((millis() - lastByteMs) > 800) {
      break;
    }
    delay(2);
  }
  client.stop();
  return true;
}

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

String extractHostForLog(const String& url) {
  int start = url.indexOf("://");
  start = (start >= 0) ? (start + 3) : 0;
  int end = url.indexOf('/', start);
  if (end < 0) {
    end = url.length();
  }
  String hostPort = url.substring(start, end);
  const int at = hostPort.lastIndexOf('@');
  if (at >= 0) {
    hostPort = hostPort.substring(at + 1);
  }
  if (hostPort.startsWith("[")) {
    const int rb = hostPort.indexOf(']');
    if (rb > 0) {
      return hostPort.substring(1, rb);
    }
  }
  const int colon = hostPort.indexOf(':');
  if (colon > 0) {
    return hostPort.substring(0, colon);
  }
  return hostPort;
}

String buildUrlWithResolvedIp(const String& url, const String& host, const String& ip) {
  if (host.isEmpty() || ip.isEmpty()) {
    return url;
  }
  int schemeEnd = url.indexOf("://");
  int hostStart = (schemeEnd >= 0) ? (schemeEnd + 3) : 0;
  int pathStart = url.indexOf('/', hostStart);
  if (pathStart < 0) {
    pathStart = url.length();
  }
  const String hostPort = url.substring(hostStart, pathStart);
  String portPart;
  const int colon = hostPort.lastIndexOf(':');
  const bool isHttps = url.startsWith("https://");
  if (colon > 0) {
    const String maybePort = hostPort.substring(colon + 1);
    bool numeric = !maybePort.isEmpty();
    for (size_t i = 0; i < maybePort.length(); ++i) {
      if (maybePort[i] < '0' || maybePort[i] > '9') {
        numeric = false;
        break;
      }
    }
    if (numeric) {
      portPart = ":" + maybePort;
    }
  } else {
    portPart = isHttps ? ":443" : ":80";
  }
  String out = url.substring(0, hostStart);
  out += ip;
  out += portPart;
  out += url.substring(pathStart);
  return out;
}

SemaphoreHandle_t sHttpMutex = nullptr;

class HttpMutexGuard {
 public:
  explicit HttpMutexGuard(uint32_t timeoutMs) {
    if (sHttpMutex == nullptr) {
      sHttpMutex = xSemaphoreCreateMutex();
    }
    if (sHttpMutex != nullptr) {
      locked_ = (xSemaphoreTake(sHttpMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE);
    }
  }

  ~HttpMutexGuard() {
    if (locked_ && sHttpMutex != nullptr) {
      xSemaphoreGive(sHttpMutex);
    }
  }

  bool locked() const { return locked_; }

 private:
  bool locked_ = false;
};
}  // namespace

bool HttpJsonClient::get(const String& url, JsonDocument& outDoc,
                         String* errorMessage, HttpFetchMeta* meta,
                         const std::map<String, String>* extraHeaders) const {
  if (meta != nullptr) {
    *meta = HttpFetchMeta();
  }
  const uint32_t startMs = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (errorMessage != nullptr) {
      *errorMessage = "WiFi disconnected, " + heapDiag();
    }
    return false;
  }

  HttpMutexGuard guard(12000);
  if (!guard.locked()) {
    if (errorMessage != nullptr) {
      *errorMessage = "HTTP busy (mutex timeout), " + heapDiag();
    }
    return false;
  }

  HTTPClient http;
  WiFiClientSecure secureClient;
  int statusCode = 0;
  const String host = extractHostForLog(url);
  IPAddress resolved;
  String connectUrl = url;
  bool useHostHeader = false;
  if (!host.isEmpty() && WiFi.hostByName(host.c_str(), resolved)) {
    const String ipText = resolved.toString();
    if (ipText.length() > 0) {
      connectUrl = buildUrlWithResolvedIp(url, host, ipText);
      useHostHeader = (connectUrl != url);
    }
  }
  if (connectUrl.startsWith("https://")) {
    secureClient.setInsecure();
    if (!http.begin(secureClient, connectUrl)) {
      if (errorMessage != nullptr) {
        *errorMessage = "HTTP begin failed";
      }
      return false;
    }
  } else {
    if (!http.begin(connectUrl)) {
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
                              "Location", "Content-Encoding", "Retry-After"};
  http.collectHeaders(headerKeys, 6);
  http.addHeader("Accept", "application/json");
  http.addHeader("User-Agent", "CoStar-ESP32/1.0");
  http.addHeader("Accept-Encoding", "identity");
  if (useHostHeader && !host.isEmpty()) {
    http.addHeader("Host", host);
  }
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
      http.addHeader(name, value);
    }
  }
  statusCode = http.GET();

  if (meta != nullptr) {
    meta->statusCode = statusCode;
    meta->elapsedMs = millis() - startMs;
  }

  if (statusCode <= 0) {
    const String transportReason = http.errorToString(statusCode);
    if (meta != nullptr) {
      meta->transportReason = transportReason;
    }
    if (errorMessage != nullptr) {
      *errorMessage = "HTTP transport failure (no HTTP response) code=" +
                      String(statusCode) + " reason='" + transportReason + "', " + heapDiag();
    }
    if (url.startsWith("https://")) {
      String fallbackBody;
      int fallbackStatus = 0;
      String fallbackType;
      int fallbackLength = -1;
      if (manualHttpsGetJson(url, extraHeaders, fallbackBody, fallbackStatus, fallbackType,
                             fallbackLength)) {
        if (meta != nullptr) {
          meta->statusCode = fallbackStatus;
          meta->contentType = fallbackType;
          meta->contentLengthBytes = fallbackLength;
          meta->payloadBytes = fallbackBody.length();
          meta->elapsedMs = millis() - startMs;
        }
        if (fallbackStatus >= 200 && fallbackStatus < 300 && !fallbackBody.isEmpty()) {
          String jsonBody = extractLikelyJson(fallbackBody);
          const DeserializationError derr = deserializeJson(outDoc, jsonBody);
          if (!derr) {
            if (errorMessage != nullptr) {
              *errorMessage = "";
            }
            http.end();
            return true;
          }
          if (errorMessage != nullptr) {
            *errorMessage = "manual https fallback JSON parse failed (" + String(derr.c_str()) +
                            "), bytes=" + String(fallbackBody.length());
          }
        } else if (errorMessage != nullptr) {
          *errorMessage = "manual https fallback status=" + String(fallbackStatus) + " preview='" +
                          compactPreview(fallbackBody) + "'";
        }
      }
    }
    http.end();
    return false;
  }

  const String contentType = http.header("Content-Type");
  const String contentLengthHeader = http.header("Content-Length");
  const String transferEncodingHeader = http.header("Transfer-Encoding");
  const String contentEncodingHeader = http.header("Content-Encoding");
  const String retryAfter = http.header("Retry-After");
  int contentLengthBytes = -1;
  if (!contentLengthHeader.isEmpty()) {
    char* endPtr = nullptr;
    const long parsed = strtol(contentLengthHeader.c_str(), &endPtr, 10);
    if (endPtr != nullptr && *endPtr == '\0' && parsed >= 0) {
      contentLengthBytes = static_cast<int>(parsed);
    }
  }

  if (statusCode < 200 || statusCode >= 300) {
    String errorPayload = http.getString();
    if (errorPayload.length() == 0) {
      errorPayload = readPayloadFromStream(http);
    }
    if (meta != nullptr) {
      meta->contentType = contentType;
      meta->contentLengthBytes = contentLengthBytes;
      meta->payloadBytes = errorPayload.length();
      meta->retryAfter = retryAfter;
      meta->elapsedMs = millis() - startMs;
    }
    if (errorMessage != nullptr) {
      *errorMessage = "HTTP status " + String(statusCode) + ", location='" +
                      http.header("Location") + "', retry-after='" + retryAfter +
                      "', preview='" + compactPreview(errorPayload, 120) + "', " +
                      heapDiag();
    }
    http.end();
    return false;
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
