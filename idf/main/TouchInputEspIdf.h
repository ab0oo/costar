#pragma once

#include <cstdint>

namespace touch_input {

struct Point {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t rawX = 0;
  uint16_t rawY = 0;
};

bool init();
bool read(Point& out);

}  // namespace touch_input
