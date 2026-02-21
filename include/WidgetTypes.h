#pragma once

#include <Arduino.h>
#include <map>

struct WidgetConfig {
  String id;
  String type;
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 120;
  int16_t h = 80;
  uint32_t updateMs = 1000;
  bool drawBorder = true;
  std::map<String, String> settings;
};
