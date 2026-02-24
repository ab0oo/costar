#include "core/TimeSync.h"

#include <ctime>

#ifdef ARDUINO
#include <Arduino.h>
#else
#include "esp_sntp.h"
#endif

#include "platform/Platform.h"

namespace {
constexpr time_t kUnixYear2000 = 946684800;
}  // namespace

namespace timesync {

void configureUtcNtp() {
#ifdef ARDUINO
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
#else
  if (esp_sntp_enabled()) {
    return;
  }
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_setservername(1, "time.nist.gov");
  esp_sntp_init();
#endif
}

bool ensureUtcTime(uint32_t timeoutMs) {
  configureUtcNtp();
  const uint32_t startMs = platform::millisMs();
  while (platform::millisMs() - startMs < timeoutMs) {
    const time_t nowUtc = time(nullptr);
    if (nowUtc > kUnixYear2000) {
      return true;
    }
    platform::sleepMs(120);
  }
  return false;
}

void logUiTimeContext(const char* timezone, int offsetMinutes, bool hasOffset) {
  configureUtcNtp();
  if (hasOffset) {
    platform::logi("time", "NTP UTC sync; local UI offset=%d min tz='%s'", offsetMinutes,
                   timezone == nullptr ? "" : timezone);
    return;
  }
  if (timezone != nullptr && *timezone != '\0') {
    platform::logi("time", "NTP UTC sync; tz='%s' (offset unknown)", timezone);
    return;
  }
  platform::logi("time", "NTP UTC sync; timezone unavailable");
}

}  // namespace timesync
