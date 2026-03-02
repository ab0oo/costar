#include "core/TimeSync.h"

#include <ctime>

#include "esp_sntp.h"

#include "platform/Platform.h"

namespace {
// Require a modern UTC epoch before treating time as synced for TLS.
constexpr time_t kUtcReadyMinUnix = 1704067200;  // 2024-01-01T00:00:00Z
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
