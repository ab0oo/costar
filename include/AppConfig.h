#pragma once

#ifdef ARDUINO
#include <Arduino.h>
#else
#include <cstdint>
#endif

namespace AppConfig {
// Migration testing mode: force both profiles to the same active layout file.
constexpr char kLayoutPathA[] = "/screen_layout_b.json";
constexpr char kLayoutPathB[] = "/screen_layout_b.json";
constexpr const char* kDefaultLayoutPath = kLayoutPathA;
constexpr float kDefaultLatitude = 37.4220f;
constexpr float kDefaultLongitude = -122.0841f;

constexpr uint32_t kLoopDelayMs = 15;
constexpr uint16_t kScreenWidth = 320;
constexpr uint16_t kScreenHeight = 240;
// Raw panel dimensions before software rotation.
constexpr uint16_t kPanelWidth = 240;
constexpr uint16_t kPanelHeight = 320;
constexpr uint8_t kRotation = 1;
constexpr int8_t kTftMisoPin = 12;
constexpr int8_t kTftMosiPin = 13;
constexpr int8_t kTftSclkPin = 14;
constexpr int8_t kTftCsPin = 15;
constexpr int8_t kTftDcPin = 2;
constexpr int8_t kTftRstPin = 12;
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
#ifdef ARDUINO
constexpr uint16_t kTouchRawMinX = 250;
constexpr uint16_t kTouchRawMaxX = 3870;
constexpr uint16_t kTouchRawMinY = 280;
constexpr uint16_t kTouchRawMaxY = 3900;
#else
// Keep calibration aligned with Arduino path to match touch/UI parity.
constexpr uint16_t kTouchRawMinX = 250;
constexpr uint16_t kTouchRawMaxX = 3870;
constexpr uint16_t kTouchRawMinY = 280;
constexpr uint16_t kTouchRawMaxY = 3900;
#endif
constexpr bool kTouchInvertX = false;
constexpr bool kTouchInvertY = false;
constexpr bool kTouchEnabled = true;
#ifdef ARDUINO
constexpr int8_t kTouchCsPin = TOUCH_CS;
constexpr int8_t kTouchIrqPin = TOUCH_IRQ;
#else
constexpr int8_t kTouchCsPin = 33;
constexpr int8_t kTouchIrqPin = 36;
#endif
constexpr int8_t kTouchSpiSckPin = 25;
constexpr int8_t kTouchSpiMisoPin = 39;
constexpr int8_t kTouchSpiMosiPin = 32;
constexpr int8_t kSdCsPin = 5;

constexpr uint16_t kWifiConnectTimeoutMs = 12000;
constexpr uint8_t kWifiScanMaxResults = 10;
constexpr uint16_t kTouchDebounceMs = 140;

// Phase-1 migration instrumentation. Keep enabled until ESP-IDF baseline is captured.
constexpr bool kBaselineMetricsEnabled = true;
constexpr uint32_t kBaselineLoopLogPeriodMs = 30000;
}
