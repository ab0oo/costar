#pragma once

#include <cstdint>

namespace layout_runtime {

bool begin(const char* layoutPath);
void tick(uint32_t nowMs);
bool onTap(uint16_t x, uint16_t y);
bool isActive();

}  // namespace layout_runtime
