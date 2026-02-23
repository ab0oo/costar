#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

#include <map>
#include <vector>

namespace dsl {

struct FormatSpec {
  int roundDigits = -100;
  String prefix;
  String suffix;
  String unit;
  String locale = "en-US";
  String tz;
  String timeFormat = "%Y-%m-%d %H:%M";
};

struct FieldSpec {
  String path;
  FormatSpec format;
};

struct TouchAction {
  String action;
  String url;
  String method = "POST";
  String body;
  String contentType = "application/json";
  String modalId;
  uint32_t dismissMs = 0;
  std::map<String, String> headers;
};

struct TouchRegion {
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;
  TouchAction onTouch;
};

struct ModalSpec {
  String id;
  String title;
  String text;
  int16_t x = -1;
  int16_t y = -1;
  int16_t w = -1;
  int16_t h = -1;
  uint8_t font = 2;
  int16_t lineHeight = 0;
  int16_t maxLines = 0;
  uint16_t textColor565 = 0xFFFF;
  uint16_t titleColor565 = 0xFFFF;
  uint16_t bgColor565 = 0x0000;
  uint16_t borderColor565 = 0x7BEF;
};

enum class NodeType : uint8_t {
  kLabel,
  kValueBox,
  kProgress,
  kSparkline,
  kIcon,
  kMoonPhase,
  kArc,
  kLine,
};

enum class OverflowMode : uint8_t {
  kClip,
  kEllipsis,
};

struct Node {
  NodeType type = NodeType::kLabel;

  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 100;
  int16_t h = 32;
  int16_t x2 = 0;
  int16_t y2 = 0;

  uint8_t font = 2;
  uint16_t color565 = 0xFFFF;
  uint16_t bg565 = 0x0000;

  String text;
  String key;
  String path;
  String angleExpr;
  uint8_t datum = TL_DATUM;
  bool wrap = false;
  int16_t lineHeight = 0;
  int16_t maxLines = 0;
  OverflowMode overflow = OverflowMode::kClip;

  float min = 0.0f;
  float max = 100.0f;
  float startDeg = 0.0f;
  float endDeg = 360.0f;
  int16_t radius = 0;
  int16_t length = 0;
  int16_t thickness = 1;
};

struct Document {
  String title = "DSL";
  String source = "http";
  String url;
  std::map<String, String> headers;
  TouchAction onTouch;
  std::vector<TouchRegion> touchRegions;
  std::vector<ModalSpec> modals;
  bool debug = false;
  uint32_t pollMs = 30000;
  std::map<String, FieldSpec> fields;
  std::vector<Node> nodes;
};

}  // namespace dsl
