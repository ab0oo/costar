#pragma once

#include <Arduino.h>

namespace AppConfig {
constexpr char kLayoutPathA[] = "/screen_layout_a.json";
constexpr char kLayoutPathB[] = "/screen_layout_b.json";
constexpr const char* kDefaultLayoutPath = kLayoutPathA;
constexpr float kDefaultLatitude = 37.4220f;
constexpr float kDefaultLongitude = -122.0841f;

constexpr uint32_t kLoopDelayMs = 15;
constexpr uint16_t kScreenWidth = 320;
constexpr uint16_t kScreenHeight = 240;
constexpr uint8_t kRotation = 1;
constexpr int8_t kBacklightPin = 21;
constexpr bool kBacklightOnHigh = true;
constexpr bool kDiagnosticMode = false;
constexpr bool kTftOnlyDiagnosticMode = false;
constexpr bool kStaticFrameMode = false;
constexpr int8_t kDiagnosticLedPin = 2;
constexpr uint16_t kDiagnosticBlinkMs = 250;
constexpr int8_t kBoardBlueLedPin = 17;
constexpr bool kBoardBlueLedOffHigh = false;
constexpr int8_t kUserButtonPin = 0;
constexpr bool kUserButtonActiveLow = true;
constexpr uint16_t kUserButtonDebounceMs = 45;

// CYD XPT2046 calibration for landscape (TFT rotation=1, touch rotation=2).
constexpr uint16_t kTouchRawMinX = 250;
constexpr uint16_t kTouchRawMaxX = 3870;
constexpr uint16_t kTouchRawMinY = 280;
constexpr uint16_t kTouchRawMaxY = 3900;
constexpr bool kTouchInvertX = false;
constexpr bool kTouchInvertY = false;
constexpr bool kTouchEnabled = true;
constexpr int8_t kTouchCsPin = TOUCH_CS;
constexpr int8_t kTouchIrqPin = TOUCH_IRQ;
constexpr int8_t kTouchSpiSckPin = 25;
constexpr int8_t kTouchSpiMisoPin = 39;
constexpr int8_t kTouchSpiMosiPin = 32;
constexpr int8_t kSdCsPin = 5;

constexpr uint16_t kWifiConnectTimeoutMs = 12000;
constexpr uint8_t kWifiScanMaxResults = 10;
constexpr uint16_t kTouchDebounceMs = 140;
}
