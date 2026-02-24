#pragma once

#include <TFT_eSPI.h>
#include <WiFi.h>
#include <XPT2046_Touchscreen.h>

#include <vector>

class WifiProvisioner {
 public:
  WifiProvisioner(TFT_eSPI& tft, XPT2046_Touchscreen& touch);
  bool connectOrProvision();

 private:
  struct NetworkInfo {
    String ssid;
    int32_t rssi = -127;
    bool secure = false;
  };

  bool tryStoredCredentials();
  bool tryConnect(const String& ssid, const String& password, bool persist);
  bool scanNetworks(std::vector<NetworkInfo>& networks);

  int pickNetwork(const std::vector<NetworkInfo>& networks);
  String promptPassword(const String& ssid);
  void drawStatus(const String& line1, const String& line2 = "");

  bool readTouch(uint16_t& x, uint16_t& y);
  void waitForTouchRelease();

  void persistCredentials(const String& ssid, const String& password);

  TFT_eSPI& tft_;
  XPT2046_Touchscreen& touch_;
};
