#pragma once

#include <cstdint>

namespace timesync {

void configureUtcNtp();
bool ensureUtcTime(uint32_t timeoutMs = 6000);
void logUiTimeContext(const char* timezone, int offsetMinutes, bool hasOffset);

}  // namespace timesync
