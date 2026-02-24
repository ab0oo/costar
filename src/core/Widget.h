#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>

#include "WidgetTypes.h"
#include "platform/Platform.h"

class Widget {
 public:
  enum class TouchType : uint8_t { kTap };

  explicit Widget(const WidgetConfig& cfg) : config_(cfg) { mutex_ = xSemaphoreCreateMutex(); }
  virtual ~Widget() {
    if (mutex_ != nullptr) {
      vSemaphoreDelete(mutex_);
      mutex_ = nullptr;
    }
  }

  virtual void begin() {
    dirty_ = true;
    // Force first update immediately instead of waiting a full polling interval.
    lastUpdateMs_ = platform::millisMs() - config_.updateMs;
  }
  virtual bool isNetworkWidget() const { return false; }
  virtual bool wantsImmediateUpdate() const { return false; }
  virtual bool onTouch(uint16_t localX, uint16_t localY, TouchType type) {
    (void)localX;
    (void)localY;
    (void)type;
    return false;
  }

  void tick(uint32_t nowMs) {
    if (mutex_ == nullptr) {
      return;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (!wantsImmediateUpdate() && (nowMs - lastUpdateMs_ < config_.updateMs)) {
      xSemaphoreGive(mutex_);
      return;
    }

    lastUpdateMs_ = nowMs;
    if (update(nowMs)) {
      dirty_ = true;
    }
    xSemaphoreGive(mutex_);
  }

  bool isDirty() const { return dirty_; }
  const WidgetConfig& config() const { return config_; }

  void clearDirty() { dirty_ = false; }
  void markDirty() { dirty_ = true; }

  bool renderIfDirty(TFT_eSPI& tft) {
    if (!dirty_ || mutex_ == nullptr) {
      return false;
    }

    if (xSemaphoreTake(mutex_, 0) != pdTRUE) {
      return false;
    }

    if (dirty_) {
      render(tft);
      dirty_ = false;
      xSemaphoreGive(mutex_);
      return true;
    }

    xSemaphoreGive(mutex_);
    return false;
  }

  void forceRender(TFT_eSPI& tft) {
    if (mutex_ != nullptr) {
      xSemaphoreTake(mutex_, portMAX_DELAY);
      render(tft);
      dirty_ = false;
      xSemaphoreGive(mutex_);
      return;
    }

    render(tft);
    dirty_ = false;
  }

  virtual bool update(uint32_t nowMs) = 0;
  virtual void render(TFT_eSPI& tft) = 0;

 protected:
  String widgetName() const {
    if (!config_.id.isEmpty()) {
      return config_.id;
    }
    return config_.type;
  }

  String logTimestamp() const {
    const time_t now = time(nullptr);
    if (now > 946684800) {
      struct tm nowTm;
      localtime_r(&now, &nowTm);
      char buf[24];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &nowTm);
      return String(buf);
    }
    return String("uptime:") + String(platform::millisMs() / 1000UL) + "s";
  }

  void logHttpFetchResult(int statusCode, int contentLengthBytes = -1) const {
    if (contentLengthBytes >= 0) {
      platform::logf("[%s] - [%s] - HTTP Fetch %d content-length=%d\n", widgetName().c_str(),
                     logTimestamp().c_str(), statusCode, contentLengthBytes);
      return;
    }
    platform::logf("[%s] - [%s] - HTTP Fetch %d content-length=unknown\n", widgetName().c_str(),
                   logTimestamp().c_str(), statusCode);
  }

  void drawPanel(TFT_eSPI& tft, const String& title) const {
    (void)title;
    tft.fillRect(config_.x, config_.y, config_.w, config_.h, TFT_BLACK);
    if (config_.drawBorder) {
      tft.drawRect(config_.x, config_.y, config_.w, config_.h, TFT_DARKGREY);
    }
  }

  const WidgetConfig config_;
  bool dirty_ = true;
  uint32_t lastUpdateMs_ = 0;
  SemaphoreHandle_t mutex_ = nullptr;
};
