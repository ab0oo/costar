#include "widgets/DslWidget.h"

#include <math.h>

void DslWidget::render(TFT_eSPI& tft) {
  drawPanel(tft, dslLoaded_ ? dsl_.title : String("DSL"));

  if (!dslLoaded_) {
    const int16_t cx = config_.x + config_.w - 6;
    const int16_t cy = config_.y + 6;
    tft.fillCircle(cx, cy, 2, TFT_RED);
    return;
  }

  for (const auto& node : dsl_.nodes) {
    const int16_t x = config_.x + node.x;
    const int16_t y = config_.y + node.y;

    if (node.type == dsl::NodeType::kLabel) {
      tft.setTextColor(node.color565, TFT_BLACK);
      tft.setTextDatum(TL_DATUM);
      tft.drawString(bindTemplate(node.text), x, y, node.font);
      continue;
    }

    if (node.type == dsl::NodeType::kValueBox) {
      tft.fillRect(x, y, node.w, node.h, node.bg565);
      tft.drawRect(x, y, node.w, node.h, node.color565);
      tft.setTextColor(node.color565, node.bg565);
      tft.setTextDatum(TL_DATUM);
      if (!node.text.isEmpty()) {
        tft.drawString(bindTemplate(node.text), x + 4, y + 4, 1);
      }
      const String value = node.key.isEmpty() ? String() : values_[node.key];
      tft.drawString(value, x + 4, y + 16, node.font);
      continue;
    }

    if (node.type == dsl::NodeType::kProgress) {
      tft.fillRect(x, y, node.w, node.h, node.bg565);
      tft.drawRect(x, y, node.w, node.h, node.color565);

      float value = 0.0f;
      if (node.key.isEmpty() || !getNumeric(node.key, value) || node.max <= node.min) {
        continue;
      }

      float ratio = (value - node.min) / (node.max - node.min);
      if (ratio < 0.0f) ratio = 0.0f;
      if (ratio > 1.0f) ratio = 1.0f;

      const int16_t innerW = node.w - 4;
      const int16_t fillW = static_cast<int16_t>(innerW * ratio);
      tft.fillRect(x + 2, y + 2, fillW, node.h - 4, node.color565);

      tft.setTextColor(TFT_WHITE, node.bg565);
      tft.setTextDatum(MC_DATUM);
      tft.drawString(String(value, 1), x + node.w / 2, y + node.h / 2, 1);
      continue;
    }

    if (node.type == dsl::NodeType::kSparkline) {
      tft.fillRect(x, y, node.w, node.h, node.bg565);
      tft.drawRect(x, y, node.w, node.h, node.color565);

      auto it = seriesValues_.find(node.key);
      if (it == seriesValues_.end() || it->second.size() < 2) {
        continue;
      }

      const std::vector<float>& s = it->second;
      float minV = node.min;
      float maxV = node.max;
      if (maxV <= minV) {
        minV = s[0];
        maxV = s[0];
        for (float v : s) {
          if (v < minV) minV = v;
          if (v > maxV) maxV = v;
        }
        if (fabsf(maxV - minV) < 0.001f) {
          maxV = minV + 1.0f;
        }
      }

      const int16_t plotW = node.w - 2;
      const int16_t plotH = node.h - 2;
      for (size_t i = 1; i < s.size(); ++i) {
        const float x0f = static_cast<float>(i - 1) / static_cast<float>(s.size() - 1);
        const float x1f = static_cast<float>(i) / static_cast<float>(s.size() - 1);
        const float y0f = (s[i - 1] - minV) / (maxV - minV);
        const float y1f = (s[i] - minV) / (maxV - minV);

        const int16_t x0 = x + 1 + static_cast<int16_t>(x0f * plotW);
        const int16_t x1 = x + 1 + static_cast<int16_t>(x1f * plotW);
        const int16_t y0 = y + node.h - 2 - static_cast<int16_t>(y0f * plotH);
        const int16_t y1 = y + node.h - 2 - static_cast<int16_t>(y1f * plotH);
        tft.drawLine(x0, y0, x1, y1, node.color565);
      }
    }

    if (node.type == dsl::NodeType::kCircle) {
      const int16_t r = node.radius > 0 ? node.radius : (node.w / 2);
      if (r <= 0) {
        continue;
      }
      if (node.bg565 != TFT_BLACK) {
        tft.fillCircle(x, y, r, node.bg565);
      }
      const int thickness = node.thickness > 0 ? node.thickness : 1;
      for (int i = 0; i < thickness; ++i) {
        tft.drawCircle(x, y, r - i, node.color565);
      }
      continue;
    }

    if (node.type == dsl::NodeType::kHand) {
      float angleDeg = 0.0f;
      bool valid = false;
      if (!node.angleExpr.isEmpty()) {
        valid = evaluateAngleExpr(node.angleExpr, angleDeg);
      } else if (!node.key.isEmpty()) {
        valid = getNumeric(node.key, angleDeg);
      }
      if (!valid) {
        continue;
      }

      const int16_t length = node.length > 0 ? node.length : node.radius;
      if (length <= 0) {
        continue;
      }

      const float radians = (angleDeg - 90.0f) * (3.14159265f / 180.0f);
      const int16_t x2 = x + static_cast<int16_t>(cosf(radians) * length);
      const int16_t y2 = y + static_cast<int16_t>(sinf(radians) * length);
      const int thickness = node.thickness > 0 ? node.thickness : 1;
      for (int i = -(thickness / 2); i <= (thickness / 2); ++i) {
        tft.drawLine(x + i, y, x2 + i, y2, node.color565);
      }
      continue;
    }
  }

  const int16_t cx = config_.x + config_.w - 6;
  const int16_t cy = config_.y + 6;
  const uint16_t color = (status_ == "ok") ? TFT_GREEN : TFT_RED;
  tft.fillCircle(cx, cy, 2, color);
}
