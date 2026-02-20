#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

struct HttpFetchMeta {
  int statusCode = 0;
  int contentLengthBytes = -1;
  size_t payloadBytes = 0;
  String contentType;
};

class HttpJsonClient {
 public:
  bool get(const String& url, JsonDocument& outDoc, String* errorMessage = nullptr,
           HttpFetchMeta* meta = nullptr) const;
};
