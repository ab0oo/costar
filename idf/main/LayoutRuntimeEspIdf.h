#pragma once

#include <cstdint>

namespace layout_runtime {

bool begin(const char* layoutPath);
void tick(uint32_t nowMs);
bool isActive();

}  // namespace layout_runtime

