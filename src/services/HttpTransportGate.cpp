#include "services/HttpTransportGate.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
SemaphoreHandle_t sTransportMutex = nullptr;
uint32_t sLastRequestMs = 0;
constexpr uint32_t kMinInterRequestGapMs = 250U;
}  // namespace

namespace httpgate {

Guard::Guard(uint32_t timeoutMs) {
  if (sTransportMutex == nullptr) {
    sTransportMutex = xSemaphoreCreateMutex();
  }
  if (sTransportMutex == nullptr) {
    return;
  }
  if (xSemaphoreTake(sTransportMutex, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
    return;
  }

  const uint32_t nowMs = millis();
  const int32_t elapsed = static_cast<int32_t>(nowMs - sLastRequestMs);
  if (elapsed >= 0 && static_cast<uint32_t>(elapsed) < kMinInterRequestGapMs) {
    delay(kMinInterRequestGapMs - static_cast<uint32_t>(elapsed));
  }
  sLastRequestMs = millis();
  locked_ = true;
}

Guard::~Guard() {
  if (locked_ && sTransportMutex != nullptr) {
    xSemaphoreGive(sTransportMutex);
  }
}

}  // namespace httpgate

