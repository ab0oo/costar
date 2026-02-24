#include "platform/Platform.h"

#include <Arduino.h>
#include <WiFi.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace platform {
namespace {

void vlogLevel(const char* level, const char* tag, const char* fmt, va_list args) {
  if (fmt == nullptr) {
    return;
  }
  if (level == nullptr) {
    level = "I";
  }
  if (tag == nullptr) {
    tag = "app";
  }

  char prefix[48];
  snprintf(prefix, sizeof(prefix), "%s (%lu) %s: ", level,
           static_cast<unsigned long>(millisMs()), tag);
  Serial.print(prefix);

  char buffer[256];
  va_list copy;
  va_copy(copy, args);
  const int n = vsnprintf(buffer, sizeof(buffer), fmt, copy);
  va_end(copy);

  if (n <= 0) {
    Serial.println();
    return;
  }
  if (n < static_cast<int>(sizeof(buffer))) {
    Serial.print(buffer);
    Serial.println();
    return;
  }

  char* dynamic = static_cast<char*>(malloc(static_cast<size_t>(n) + 1U));
  if (dynamic == nullptr) {
    Serial.print(buffer);
    Serial.println();
    return;
  }
  vsnprintf(dynamic, static_cast<size_t>(n) + 1U, fmt, args);
  Serial.print(dynamic);
  Serial.println();
  free(dynamic);
}

}  // namespace

void serialBegin(uint32_t baudRate) { Serial.begin(baudRate); }

uint32_t millisMs() { return millis(); }

void sleepMs(uint32_t ms) { delay(ms); }

void log(const char* msg) { Serial.println(msg); }

void logf(const char* fmt, ...) {
  if (fmt == nullptr) {
    return;
  }
  char buffer[256];
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  if (n <= 0) {
    return;
  }
  if (n < static_cast<int>(sizeof(buffer))) {
    Serial.print(buffer);
    return;
  }

  // Preserve full messages that exceed stack buffer size.
  char* dynamic = static_cast<char*>(malloc(static_cast<size_t>(n) + 1U));
  if (dynamic == nullptr) {
    Serial.print(buffer);
    return;
  }
  va_start(args, fmt);
  vsnprintf(dynamic, static_cast<size_t>(n) + 1U, fmt, args);
  va_end(args);
  Serial.print(dynamic);
  free(dynamic);
}

void logi(const char* tag, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vlogLevel("I", tag, fmt, args);
  va_end(args);
}

void logw(const char* tag, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vlogLevel("W", tag, fmt, args);
  va_end(args);
}

void loge(const char* tag, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vlogLevel("E", tag, fmt, args);
  va_end(args);
}

uint32_t freeHeapBytes() { return ESP.getFreeHeap(); }

uint32_t minFreeHeapBytes() { return ESP.getMinFreeHeap(); }

int wifiRssi() { return WiFi.RSSI(); }

}  // namespace platform
