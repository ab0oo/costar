#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <map>

struct HttpFetchMeta {
  int statusCode = 0;
  int contentLengthBytes = -1;
  size_t payloadBytes = 0;
  String contentType;
  String transportReason;
  String retryAfter;
  uint32_t elapsedMs = 0;
};

class HttpJsonClient {
 public:
  bool get(const String& url, JsonDocument& outDoc, String* errorMessage = nullptr,
           HttpFetchMeta* meta = nullptr,
           const std::map<String, String>* extraHeaders = nullptr) const;
};
