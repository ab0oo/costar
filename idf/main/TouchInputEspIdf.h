#pragma once

#include <cstdint>

namespace touch_input {

struct Point {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t rawX = 0;
  uint16_t rawY = 0;
};

struct Calibration {
  uint16_t rawMinX = 0;
  uint16_t rawMaxX = 0;
  uint16_t rawMinY = 0;
  uint16_t rawMaxY = 0;
  bool swapXY = true;
  bool invertX = false;
  bool invertY = false;
  int16_t xCorrLeft = 0;
  int16_t xCorrRight = 0;
  int16_t yCorr = 0;
};

bool init();
bool read(Point& out);
bool hasCalibration();
bool loadCalibration(Calibration& out);
bool saveCalibration(const Calibration& calibration);
void setCalibration(const Calibration& calibration);
void getCalibration(Calibration& out);

}  // namespace touch_input
