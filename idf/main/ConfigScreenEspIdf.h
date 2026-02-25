#pragma once

#include <cstdint>

namespace config_screen {

enum class Action : uint8_t {
  None = 0,
  RetryWifi,
  OfflineMode,
  OpenWifiList,
  ToggleClock,
  ToggleTemp,
  ToggleDistance,
};

struct ViewState {
  bool hasStoredCreds = false;
  bool wifiConnected = false;
  bool showWifiButtons = false;
  bool use24HourClock = false;
  bool useFahrenheit = true;
  bool useMiles = true;
};

void show(const ViewState& state);
void showWifiScanInterstitial();
void showWifiStatus(const char* title, const char* subtitle, bool isError);
Action hitTest(uint16_t x, uint16_t y);
void showWifiList(const char* const* labels, uint16_t count);
int hitTestWifiListRow(uint16_t x, uint16_t y, uint16_t count);
void markTouch(uint16_t x, uint16_t y);

}  // namespace config_screen
