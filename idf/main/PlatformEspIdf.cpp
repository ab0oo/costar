#include "platform/Platform.h"

#include <cstdarg>
#include <cstdio>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace platform {
namespace {

void vlogWithLevel(esp_log_level_t level, const char* tag, const char* fmt, va_list args) {
  if (fmt == nullptr) {
    return;
  }
  const char* resolvedTag = (tag == nullptr || *tag == '\0') ? "app" : tag;
  char buffer[512];
  va_list copy;
  va_copy(copy, args);
  const int n = std::vsnprintf(buffer, sizeof(buffer), fmt, copy);
  va_end(copy);
  if (n <= 0) {
    return;
  }

  if (n < static_cast<int>(sizeof(buffer))) {
    ESP_LOG_LEVEL(level, resolvedTag, "%s", buffer);
    return;
  }

  char* dynamic = new (std::nothrow) char[static_cast<size_t>(n) + 1U];
  if (dynamic == nullptr) {
    ESP_LOG_LEVEL(level, resolvedTag, "%s", buffer);
    return;
  }
  std::vsnprintf(dynamic, static_cast<size_t>(n) + 1U, fmt, args);
  ESP_LOG_LEVEL(level, resolvedTag, "%s", dynamic);
  delete[] dynamic;
}

}  // namespace

void serialBegin(uint32_t baudRate) { (void)baudRate; }

uint32_t millisMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL); }

void sleepMs(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

void log(const char* msg) {
  if (msg == nullptr) {
    return;
  }
  ESP_LOGI("app", "%s", msg);
}

void logf(const char* fmt, ...) {
  if (fmt == nullptr) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  vlogWithLevel(ESP_LOG_INFO, "app", fmt, args);
  va_end(args);
}

void logi(const char* tag, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vlogWithLevel(ESP_LOG_INFO, tag, fmt, args);
  va_end(args);
}

void logw(const char* tag, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vlogWithLevel(ESP_LOG_WARN, tag, fmt, args);
  va_end(args);
}

void loge(const char* tag, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vlogWithLevel(ESP_LOG_ERROR, tag, fmt, args);
  va_end(args);
}

uint32_t freeHeapBytes() {
  return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_8BIT));
}

uint32_t minFreeHeapBytes() {
  return static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

int wifiRssi() {
  wifi_ap_record_t apInfo = {};
  if (esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK) {
    return static_cast<int>(apInfo.rssi);
  }
  return 0;
}

}  // namespace platform
