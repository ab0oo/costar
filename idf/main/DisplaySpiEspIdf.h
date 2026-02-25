#pragma once

#include <cstdint>

namespace display_spi {

bool init();
bool initPanel();
bool applyPanelTuning(bool bgr, bool invert, bool persist);
bool drawSanityPattern();
bool fillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color565);
bool drawRgb565(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t* pixels);
bool clear(uint16_t color565 = 0x0000);
uint16_t width();
uint16_t height();

}  // namespace display_spi
