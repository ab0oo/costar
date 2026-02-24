#include "core/DisplayManager.h"

#include <ArduinoJson.h>

#include "AppConfig.h"
#include "core/WidgetFactory.h"
#include "platform/Fs.h"
#include "platform/Platform.h"
#include "widgets/DslRuntimeCaches.h"

DisplayManager::DisplayManager(TFT_eSPI& tft, const String& layoutPath)
    : tft_(tft), layoutPath_(layoutPath) {
  widgetsMutex_ = xSemaphoreCreateMutex();
}

DisplayManager::~DisplayManager() {
  if (networkTaskHandle_ != nullptr) {
    vTaskDelete(networkTaskHandle_);
    networkTaskHandle_ = nullptr;
  }
  if (widgetsMutex_ != nullptr) {
    vSemaphoreDelete(widgetsMutex_);
    widgetsMutex_ = nullptr;
  }
}

bool DisplayManager::begin() {
  // TFT is initialized once in setup(); avoid re-initializing controller state.
  tft_.fillScreen(TFT_BLACK);
  tft_.setTextFont(2);

  drawBootMessage("Widget OS", "Loading layout...");
  if (!loadLayout()) {
    return false;
  }

  if (networkTaskHandle_ == nullptr) {
    xTaskCreatePinnedToCore(DisplayManager::networkTaskEntry, "widget-net", 8192, this, 1,
                            &networkTaskHandle_, 0);
  }
  return true;
}

void DisplayManager::loop(uint32_t nowMs) {
  if (widgetsMutex_ == nullptr) {
    return;
  }

  xSemaphoreTake(widgetsMutex_, portMAX_DELAY);
  for (const auto& widget : widgets_) {
    if (!widget->isNetworkWidget()) {
      widget->tick(nowMs);
    }
    widget->renderIfDirty(tft_);
  }

  if (touchOverlay_) {
    tft_.drawRect(0, 0, AppConfig::kScreenWidth, AppConfig::kScreenHeight, TFT_MAGENTA);
    tft_.setTextColor(TFT_MAGENTA, TFT_BLACK);
    tft_.drawString("Touch debug ON", 4, AppConfig::kScreenHeight - 16, 2);
  }
  xSemaphoreGive(widgetsMutex_);
}

bool DisplayManager::reloadLayout() {
  return loadLayout();
}

bool DisplayManager::reloadLayout(const String& layoutPath) {
  layoutPath_ = layoutPath;
  return loadLayout();
}

void DisplayManager::setLayoutPath(const String& layoutPath) {
  layoutPath_ = layoutPath;
}

void DisplayManager::onTouch(uint16_t rawX, uint16_t rawY) {
  if (widgetsMutex_ == nullptr) {
    return;
  }

  xSemaphoreTake(widgetsMutex_, portMAX_DELAY);
  bool handled = false;

  // Dispatch to top-most region first.
  for (auto it = widgets_.rbegin(); it != widgets_.rend(); ++it) {
    Widget* widget = it->get();
    if (widget == nullptr) {
      continue;
    }
    const WidgetConfig& cfg = widget->config();
    if (rawX < cfg.x || rawY < cfg.y) {
      continue;
    }
    if (rawX >= static_cast<uint16_t>(cfg.x + cfg.w) ||
        rawY >= static_cast<uint16_t>(cfg.y + cfg.h)) {
      continue;
    }

    const uint16_t localX = rawX - cfg.x;
    const uint16_t localY = rawY - cfg.y;
    if (widget->onTouch(localX, localY, Widget::TouchType::kTap)) {
      handled = true;
      widget->markDirty();
      break;
    }
  }

  if (!handled) {
    // Keep an escape hatch for touch diagnostics.
    touchOverlay_ = !touchOverlay_;
    if (!touchOverlay_) {
      tft_.fillRect(0, AppConfig::kScreenHeight - 20, AppConfig::kScreenWidth, 20, TFT_BLACK);
      for (const auto& widget : widgets_) {
        widget->forceRender(tft_);
      }
    }
  }
  xSemaphoreGive(widgetsMutex_);
}

bool DisplayManager::loadLayout() {
  if (widgetsMutex_ == nullptr) {
    return false;
  }

  xSemaphoreTake(widgetsMutex_, portMAX_DELAY);
  widgets_.clear();
  clearDslRuntimeCaches();

  platform::fs::File manifest = platform::fs::open(layoutPath_, FILE_READ);
  if (!manifest || manifest.isDirectory()) {
    xSemaphoreGive(widgetsMutex_);
    drawBootMessage("Layout missing", layoutPath_);
    return false;
  }

  JsonDocument doc;
  const DeserializationError parseError = deserializeJson(doc, manifest);
  manifest.close();

  if (parseError) {
    xSemaphoreGive(widgetsMutex_);
    drawBootMessage("Layout parse err", parseError.f_str());
    return false;
  }

  const JsonObjectConst screen = doc["screen"];
  const JsonObjectConst widgetDefs = doc["widget_defs"];
  const JsonArrayConst regions = screen["regions"];

  if (screen.isNull()) {
    xSemaphoreGive(widgetsMutex_);
    drawBootMessage("Layout invalid", "Missing 'screen' object");
    return false;
  }
  if (widgetDefs.isNull()) {
    xSemaphoreGive(widgetsMutex_);
    drawBootMessage("Layout invalid", "Missing 'widget_defs' object");
    return false;
  }
  if (regions.isNull() || regions.size() == 0) {
    xSemaphoreGive(widgetsMutex_);
    drawBootMessage("Layout empty", "No regions found");
    return false;
  }

  for (JsonObjectConst region : regions) {
    WidgetConfig cfg;
    if (!parseRegionConfig(region, widgetDefs, cfg)) {
      continue;
    }

    std::unique_ptr<Widget> widget = WidgetFactory::create(cfg);
    if (widget == nullptr) {
      continue;
    }

    widget->begin();
    widgets_.push_back(std::move(widget));
  }

  if (widgets_.empty()) {
    xSemaphoreGive(widgetsMutex_);
    drawBootMessage("Layout invalid", "No valid regions/widgets");
    return false;
  }

  tft_.fillScreen(TFT_BLACK);
  for (const auto& widget : widgets_) {
    widget->forceRender(tft_);
  }
  xSemaphoreGive(widgetsMutex_);
  return true;
}

void DisplayManager::networkTaskEntry(void* arg) {
  if (arg == nullptr) {
    vTaskDelete(nullptr);
    return;
  }
  static_cast<DisplayManager*>(arg)->networkTaskLoop();
}

void DisplayManager::networkTaskLoop() {
  if (widgetsMutex_ == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  for (;;) {
    const uint32_t nowMs = platform::millisMs();
    xSemaphoreTake(widgetsMutex_, portMAX_DELAY);
    for (const auto& widget : widgets_) {
      if (widget->isNetworkWidget()) {
        widget->tick(nowMs);
      }
    }
    xSemaphoreGive(widgetsMutex_);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

bool DisplayManager::parseWidgetConfig(const JsonObjectConst& node, WidgetConfig& outCfg) const {
  if (node["type"].isNull()) {
    return false;
  }

  outCfg.type = node["type"].as<const char*>();
  outCfg.id = node["id"] | outCfg.type;
  outCfg.x = node["x"] | 0;
  outCfg.y = node["y"] | 0;
  outCfg.w = node["w"] | 120;
  outCfg.h = node["h"] | 80;
  outCfg.updateMs = node["update_ms"] | 1000;
  outCfg.drawBorder = node["draw_border"] | outCfg.drawBorder;

  const JsonObjectConst settings = node["settings"];
  if (!settings.isNull()) {
    for (JsonPairConst item : settings) {
      outCfg.settings[String(item.key().c_str())] = String(item.value().as<String>());
    }
  }

  return true;
}

bool DisplayManager::parseRegionConfig(const JsonObjectConst& region,
                                       const JsonObjectConst& widgetDefs,
                                       WidgetConfig& outCfg) const {
  const String ref = region["widget"] | String();
  if (ref.isEmpty() || widgetDefs.isNull()) {
    return false;
  }

  const JsonObjectConst def = widgetDefs[ref];
  if (def.isNull()) {
    return false;
  }

  if (!parseWidgetConfig(def, outCfg)) {
    return false;
  }

  // Region coordinates are authoritative for placement and boundaries.
  outCfg.x = region["x"] | outCfg.x;
  outCfg.y = region["y"] | outCfg.y;
  outCfg.w = region["w"] | outCfg.w;
  outCfg.h = region["h"] | outCfg.h;
  outCfg.drawBorder = region["draw_border"] | outCfg.drawBorder;

  const String regionId = region["id"] | String();
  if (!regionId.isEmpty()) {
    outCfg.id = regionId;
  }

  return true;
}

void DisplayManager::drawBootMessage(const String& line1, const String& line2) {
  tft_.fillScreen(TFT_BLACK);
  tft_.setTextDatum(MC_DATUM);
  tft_.setTextColor(TFT_CYAN, TFT_BLACK);
  tft_.drawString(line1, AppConfig::kScreenWidth / 2, AppConfig::kScreenHeight / 2 - 16, 2);
  tft_.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  if (line2.length() > 0) {
    tft_.drawString(line2, AppConfig::kScreenWidth / 2, AppConfig::kScreenHeight / 2 + 8, 2);
  }
  tft_.setTextDatum(TL_DATUM);
}
