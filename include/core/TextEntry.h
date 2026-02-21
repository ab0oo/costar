#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

struct TextEntryOptions {
  String title;
  String subtitle;
  String initial;
  bool numericOnly = false;
  bool maskInput = false;
  uint8_t maxLen = 63;
};

class TextEntry {
 public:
  TextEntry(TFT_eSPI& tft, XPT2046_Touchscreen& touch);
  String prompt(const TextEntryOptions& options);

 private:
  bool readTouch(uint16_t& x, uint16_t& y);
  void waitForTouchRelease();

  TFT_eSPI& tft_;
  XPT2046_Touchscreen& touch_;
};
