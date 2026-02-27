#pragma once

#include <cstdint>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace http_transport_gate {

inline SemaphoreHandle_t sHttpTransportGate = nullptr;
inline portMUX_TYPE sHttpTransportGateInitMux = portMUX_INITIALIZER_UNLOCKED;
inline constexpr const char* kHttpGateTag = "http-gate";

inline SemaphoreHandle_t ensureGate() {
  if (sHttpTransportGate != nullptr) {
    return sHttpTransportGate;
  }
  taskENTER_CRITICAL(&sHttpTransportGateInitMux);
  if (sHttpTransportGate == nullptr) {
    sHttpTransportGate = xSemaphoreCreateMutex();
  }
  SemaphoreHandle_t gate = sHttpTransportGate;
  taskEXIT_CRITICAL(&sHttpTransportGateInitMux);
  return gate;
}

inline bool take(uint32_t timeoutMs) {
  SemaphoreHandle_t gate = ensureGate();
  if (gate == nullptr) {
    ESP_LOGE(kHttpGateTag, "gate alloc failed");
    return false;
  }
  if (xSemaphoreTake(gate, 0) == pdTRUE) {
    return true;
  }

  const TickType_t startTick = xTaskGetTickCount();
  ESP_LOGW(kHttpGateTag, "waiting for in-flight HTTP request timeout_ms=%u",
           static_cast<unsigned>(timeoutMs));
  if (xSemaphoreTake(gate, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
    ESP_LOGW(kHttpGateTag, "wait timeout timeout_ms=%u", static_cast<unsigned>(timeoutMs));
    return false;
  }

  const TickType_t waitedTicks = xTaskGetTickCount() - startTick;
  ESP_LOGW(kHttpGateTag, "acquired after wait_ms=%u", static_cast<unsigned>(pdTICKS_TO_MS(waitedTicks)));
  return true;
}

inline void give() {
  if (sHttpTransportGate != nullptr) {
    (void)xSemaphoreGive(sHttpTransportGate);
  }
}

}  // namespace http_transport_gate
