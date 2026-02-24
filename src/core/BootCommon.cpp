#include "core/BootCommon.h"

#include "RuntimeSettings.h"
#include "platform/Platform.h"

namespace boot {

void start(BaselineState& state) {
  state.bootStartMs = platform::millisMs();
  state.lastLoopLogMs = 0;
}

void mark(BaselineState& state, const char* stage, bool enabled) {
  if (!enabled || stage == nullptr) {
    return;
  }
  const unsigned long nowMs = platform::millisMs();
  const unsigned long elapsedMs = nowMs - state.bootStartMs;
  platform::logi("baseline", "stage=%s t_ms=%lu heap_free=%u heap_min=%u", stage, elapsedMs,
                 platform::freeHeapBytes(), platform::minFreeHeapBytes());
}

void markLoop(BaselineState& state, bool wifiReady, bool enabled, unsigned long periodMs) {
  if (!enabled) {
    return;
  }
  const unsigned long nowMs = platform::millisMs();
  if (state.lastLoopLogMs == 0) {
    state.lastLoopLogMs = nowMs;
    return;
  }
  if (nowMs - state.lastLoopLogMs < periodMs) {
    return;
  }
  state.lastLoopLogMs = nowMs;
  platform::logi("baseline", "uptime_s=%lu heap_free=%u heap_min=%u wifi=%d rssi=%d",
                 nowMs / 1000UL, platform::freeHeapBytes(), platform::minFreeHeapBytes(),
                 wifiReady ? 1 : 0, wifiReady ? platform::wifiRssi() : 0);
}

void logSettingsSummary(bool includeAdsbRadius) {
  if (includeAdsbRadius) {
    platform::logi("settings", "clock=%s temp=%s dist=%s adsb=%unm",
                   RuntimeSettings::use24HourClock ? "24h" : "12h",
                   RuntimeSettings::useFahrenheit ? "F" : "C",
                   RuntimeSettings::useMiles ? "mi" : "km", RuntimeSettings::adsbRadiusNm);
    return;
  }

  platform::logi("settings", "clock=%s temp=%s dist=%s",
                 RuntimeSettings::use24HourClock ? "24h" : "12h",
                 RuntimeSettings::useFahrenheit ? "F" : "C",
                 RuntimeSettings::useMiles ? "mi" : "km");
}

}  // namespace boot
