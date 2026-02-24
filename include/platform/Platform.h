#pragma once

#include <cstdint>

namespace platform {

void serialBegin(uint32_t baudRate);

uint32_t millisMs();
void sleepMs(uint32_t ms);

void log(const char* msg);
void logf(const char* fmt, ...);
void logi(const char* tag, const char* fmt, ...);
void logw(const char* tag, const char* fmt, ...);
void loge(const char* tag, const char* fmt, ...);

uint32_t freeHeapBytes();
uint32_t minFreeHeapBytes();
int wifiRssi();

}  // namespace platform
