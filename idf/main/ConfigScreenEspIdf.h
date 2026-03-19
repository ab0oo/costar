#pragma once

#include <cstdint>

namespace config_screen {

enum class Action : uint8_t {
  None = 0,
  ToggleClock,
  ToggleTemp,
  ToggleDistance,
};

struct ViewState {
  bool wifiConnected = false;
  bool use24HourClock = false;
  bool useFahrenheit = true;
  bool useMiles = true;
};

// Draw the locale config screen (time/temp/distance toggles + wifi status line).
void show(const ViewState& state);

// Draw a full-screen status message used during boot (connecting, failed, etc.).
void showWifiStatus(const char* title, const char* subtitle, bool isError);

// Hit-test a touch point against the locale toggle buttons.
Action hitTest(uint16_t x, uint16_t y);

// Draw a small yellow touch-marker dot (used during calibration / debug).
void markTouch(uint16_t x, uint16_t y);

}  // namespace config_screen
