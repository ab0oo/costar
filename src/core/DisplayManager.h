#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <memory>
#include <vector>

#include "Widget.h"

class DisplayManager {
 public:
  DisplayManager(TFT_eSPI& tft, const String& layoutPath);
  ~DisplayManager();

  bool begin();
  void loop(uint32_t nowMs);
  bool reloadLayout();
  void onTouch(uint16_t rawX, uint16_t rawY);

 private:
  static void networkTaskEntry(void* arg);
  void networkTaskLoop();
  bool loadLayout();
  bool parseWidgetConfig(const JsonObjectConst& node, WidgetConfig& outCfg) const;
  bool parseRegionConfig(const JsonObjectConst& region, const JsonObjectConst& widgetDefs,
                         WidgetConfig& outCfg) const;
  void drawBootMessage(const String& line1, const String& line2 = "");

  TFT_eSPI& tft_;
  String layoutPath_;
  std::vector<std::unique_ptr<Widget>> widgets_;
  bool touchOverlay_ = false;
  TaskHandle_t networkTaskHandle_ = nullptr;
  SemaphoreHandle_t widgetsMutex_ = nullptr;
};
