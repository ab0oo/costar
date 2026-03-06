#include "core/TimeSync.h"

#include <ctime>

#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "platform/Platform.h"

namespace {
// Require a modern UTC epoch before treating time as synced for TLS.
constexpr time_t kUtcReadyMinUnix = 1704067200;  // 2024-01-01T00:00:00Z
constexpr uint32_t kUtcBackgroundProbeWindowMs = 15000;
constexpr uint32_t kUtcBackgroundProbeSleepMs = 250;
constexpr uint32_t kUtcBackgroundRetryBackoffMs = 2000;
constexpr uint32_t kUtcBackgroundSteadySleepMs = 60000;
TaskHandle_t sUtcRetryTask = nullptr;

void utcRetryTask(void* arg) {
  (void)arg;
  uint32_t attempt = 0;
  bool loggedReady = false;
  for (;;) {
    if (timesync::isUtcTimeReady()) {
      if (!loggedReady) {
        loggedReady = true;
        platform::logi("time", "UTC time ready via background retry");
      }
      platform::sleepMs(kUtcBackgroundSteadySleepMs);
      continue;
    }

    loggedReady = false;
    ++attempt;
    platform::logi("time", "UTC time retry attempt=%u", static_cast<unsigned>(attempt));
    const uint32_t startMs = platform::millisMs();
    while (platform::millisMs() - startMs < kUtcBackgroundProbeWindowMs) {
      if (timesync::isUtcTimeReady()) {
        break;
      }
      platform::sleepMs(kUtcBackgroundProbeSleepMs);
    }

    if (!timesync::isUtcTimeReady()) {
      platform::logw("time", "UTC time still unsynced after attempt=%u; will keep retrying",
                     static_cast<unsigned>(attempt));
      platform::sleepMs(kUtcBackgroundRetryBackoffMs);
    }
  }
}

void ensureUtcRetryTaskStarted() {
  if (sUtcRetryTask != nullptr) {
    return;
  }
  if (xTaskCreatePinnedToCore(utcRetryTask, "utc_retry", 3072, nullptr, 1, &sUtcRetryTask, 0) == pdPASS) {
    platform::logi("time", "UTC retry task started");
  } else {
    platform::logw("time", "failed to start UTC retry task");
  }
}
}  // namespace

namespace timesync {

void configureUtcNtp() {
  if (esp_sntp_enabled()) {
    return;
  }
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_setservername(1, "time.nist.gov");
  esp_sntp_init();
}

bool ensureUtcTime(uint32_t timeoutMs) {
  configureUtcNtp();
  const uint32_t startMs = platform::millisMs();
  while (platform::millisMs() - startMs < timeoutMs) {
    if (isUtcTimeReady()) {
      return true;
    }
    platform::sleepMs(120);
  }
  // Don't give up after the boot wait window; keep retrying in the background.
  ensureUtcRetryTaskStarted();
  return false;
}

bool isUtcTimeReady() { return time(nullptr) >= kUtcReadyMinUnix; }

void logUiTimeContext(const char* timezone, int offsetMinutes, bool hasOffset) {
  configureUtcNtp();
  const bool ready = isUtcTimeReady();
  if (hasOffset) {
    platform::logi("time", "UTC time %s; local UI offset=%d min tz='%s'",
                   ready ? "ready" : "not yet synced",
                   offsetMinutes, timezone == nullptr ? "" : timezone);
    return;
  }
  if (timezone != nullptr && *timezone != '\0') {
    platform::logi("time", "UTC time %s; tz='%s' (offset unknown)",
                   ready ? "ready" : "not yet synced",
                   timezone);
    return;
  }
  platform::logi("time", "UTC time %s; timezone unavailable", ready ? "ready" : "not yet synced");
}

}  // namespace timesync
