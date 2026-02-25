#pragma once

#include <cstdint>

namespace dsl_widget_runtime {

void reset();
bool begin(const char* widgetId, const char* dslPath, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void tick(uint32_t nowMs);
bool isActive();

}  // namespace dsl_widget_runtime

