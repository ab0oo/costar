#pragma once

#include <Arduino.h>
#include <XPT2046_Touchscreen.h>

#include "AppConfig.h"

struct TouchPoint {
  uint16_t x = 0;
  uint16_t y = 0;
};

class TouchMapper {
 public:
  static bool mapRaw(const TS_Point& raw, TouchPoint& out) {
    // For CYD with touch rotation=2:
    // raw.x tracks screen Y, raw.y tracks screen X (inverted).
    long y = map(raw.x, AppConfig::kTouchRawMinX, AppConfig::kTouchRawMaxX, 0,
                 AppConfig::kScreenHeight - 1);
    long x = map(raw.y, AppConfig::kTouchRawMaxY, AppConfig::kTouchRawMinY, 0,
                 AppConfig::kScreenWidth - 1);

    x = constrain(x, 0, AppConfig::kScreenWidth - 1);
    y = constrain(y, 0, AppConfig::kScreenHeight - 1);

    if (AppConfig::kTouchInvertX) {
      x = (AppConfig::kScreenWidth - 1) - x;
    }
    if (AppConfig::kTouchInvertY) {
      y = (AppConfig::kScreenHeight - 1) - y;
    }

    out.x = static_cast<uint16_t>(x);
    out.y = static_cast<uint16_t>(y);
    return true;
  }
};
