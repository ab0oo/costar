#pragma once

#include <cstdint>

namespace dsl_widget_runtime {

void reset();
bool begin(const char* widgetId, const char* dslPath, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
           const char* settingsJson = nullptr, const char* sharedSettingsJson = nullptr);
bool tick(uint32_t nowMs);
bool onTap(const char* widgetId, uint16_t localX, uint16_t localY);
bool isActive();

}  // namespace dsl_widget_runtime
