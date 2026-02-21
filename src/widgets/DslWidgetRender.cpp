#include "widgets/DslWidget.h"

#include <math.h>
#include <utility>
#include <vector>

#include <FS.h>
#include <LittleFS.h>

namespace {
struct IconCacheEntry {
  String key;
  int16_t w = 0;
  int16_t h = 0;
  std::vector<uint16_t> pixels;
};

constexpr size_t kMaxIconCache = 12;
std::vector<IconCacheEntry> sIconCache;

const IconCacheEntry* findIcon(const String& key) {
  for (const auto& entry : sIconCache) {
    if (entry.key == key) {
      return &entry;
    }
  }
  return nullptr;
}

const IconCacheEntry* loadIcon(const String& path, int16_t w, int16_t h) {
  if (path.isEmpty() || w <= 0 || h <= 0) {
    return nullptr;
  }
  const String key = path + "#" + String(w) + "x" + String(h);
  if (const IconCacheEntry* cached = findIcon(key)) {
    return cached;
  }

  fs::File f = LittleFS.open(path, FILE_READ);
  if (!f || f.isDirectory()) {
    return nullptr;
  }

  const size_t expected = static_cast<size_t>(w) * static_cast<size_t>(h) * 2U;
  if (f.size() < expected) {
    f.close();
    return nullptr;
  }

  IconCacheEntry entry;
  entry.key = key;
  entry.w = w;
  entry.h = h;
  entry.pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
  const size_t read =
      f.read(reinterpret_cast<uint8_t*>(entry.pixels.data()), expected);
  f.close();
  if (read != expected) {
    return nullptr;
  }

  if (sIconCache.size() >= kMaxIconCache) {
    sIconCache.erase(sIconCache.begin());
  }
  sIconCache.push_back(std::move(entry));
  return &sIconCache.back();
}
}  // namespace

void DslWidget::render(TFT_eSPI& tft) {
  auto drawPanelTo = [&](auto& gfx) {
    gfx.fillRect(0, 0, config_.w, config_.h, TFT_BLACK);
    if (config_.drawBorder) {
      gfx.drawRect(0, 0, config_.w, config_.h, TFT_DARKGREY);
    }
  };

  auto renderNodes = [&](auto& gfx, int16_t baseX, int16_t baseY) {
    for (const auto& node : dsl_.nodes) {
      const int16_t x = baseX + node.x;
      const int16_t y = baseY + node.y;

      if (node.type == dsl::NodeType::kLabel) {
        gfx.setTextColor(node.color565, TFT_BLACK);
        gfx.setTextDatum(node.datum);
        gfx.drawString(bindTemplate(node.text), x, y, node.font);
        continue;
      }

      if (node.type == dsl::NodeType::kValueBox) {
        gfx.fillRect(x, y, node.w, node.h, node.bg565);
        gfx.drawRect(x, y, node.w, node.h, node.color565);
        gfx.setTextColor(node.color565, node.bg565);
        gfx.setTextDatum(TL_DATUM);
        if (!node.text.isEmpty()) {
          gfx.drawString(bindTemplate(node.text), x + 4, y + 4, 1);
        }
        const String value = node.key.isEmpty() ? String() : values_[node.key];
        gfx.drawString(value, x + 4, y + 16, node.font);
        continue;
      }

      if (node.type == dsl::NodeType::kProgress) {
        gfx.fillRect(x, y, node.w, node.h, node.bg565);
        gfx.drawRect(x, y, node.w, node.h, node.color565);

        float value = 0.0f;
        if (node.key.isEmpty() || !getNumeric(node.key, value) || node.max <= node.min) {
          continue;
        }

        float ratio = (value - node.min) / (node.max - node.min);
        if (ratio < 0.0f) ratio = 0.0f;
        if (ratio > 1.0f) ratio = 1.0f;

        const int16_t innerW = node.w - 4;
        const int16_t fillW = static_cast<int16_t>(innerW * ratio);
        gfx.fillRect(x + 2, y + 2, fillW, node.h - 4, node.color565);

        gfx.setTextColor(TFT_WHITE, node.bg565);
        gfx.setTextDatum(MC_DATUM);
        gfx.drawString(String(value, 1), x + node.w / 2, y + node.h / 2, 1);
        continue;
      }

      if (node.type == dsl::NodeType::kSparkline) {
        gfx.fillRect(x, y, node.w, node.h, node.bg565);
        gfx.drawRect(x, y, node.w, node.h, node.color565);

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
          gfx.drawLine(x0, y0, x1, y1, node.color565);
        }
        continue;
      }

    if (node.type == dsl::NodeType::kArc) {
      const int16_t r = node.radius > 0 ? node.radius : (node.w / 2);
      if (r <= 0) {
        continue;
      }
      const float startDeg = node.startDeg;
      const float endDeg = node.endDeg;
      const float span = fabsf(endDeg - startDeg);
      if (span >= 359.0f && node.bg565 != TFT_BLACK) {
        gfx.fillCircle(x, y, r, node.bg565);
      }
      const int thickness = node.thickness > 0 ? node.thickness : 1;
      const float step = span > 120.0f ? 2.0f : 1.0f;
      for (int t = 0; t < thickness; ++t) {
        const int rr = r - t;
        for (float a = startDeg; a <= endDeg; a += step) {
          const float rad = (a - 90.0f) * (3.14159265f / 180.0f);
          const int16_t px = x + static_cast<int16_t>(cosf(rad) * rr);
          const int16_t py = y + static_cast<int16_t>(sinf(rad) * rr);
          gfx.drawPixel(px, py, node.color565);
        }
      }
      continue;
    }

    if (node.type == dsl::NodeType::kLine) {
      float angleDeg = 0.0f;
      bool useAngle = false;
      if (!node.angleExpr.isEmpty()) {
        useAngle = evaluateAngleExpr(node.angleExpr, angleDeg);
      } else if (!node.key.isEmpty()) {
        useAngle = getNumeric(node.key, angleDeg);
      }

      int16_t x2 = node.x2;
      int16_t y2 = node.y2;
      if (useAngle) {
        const int16_t length = node.length > 0 ? node.length : node.radius;
        if (length <= 0) {
          continue;
        }
        const float radians = (angleDeg - 90.0f) * (3.14159265f / 180.0f);
        x2 = x + static_cast<int16_t>(cosf(radians) * length);
        y2 = y + static_cast<int16_t>(sinf(radians) * length);
      } else {
        x2 = baseX + node.x2;
        y2 = baseY + node.y2;
      }

      const int thickness = node.thickness > 0 ? node.thickness : 1;
      const float dx = static_cast<float>(x2 - x);
      const float dy = static_cast<float>(y2 - y);
      const float len = sqrtf(dx * dx + dy * dy);
      if (len < 0.0001f) {
        continue;
      }
      const float nx = -dy / len;
      const float ny = dx / len;
      for (int i = -(thickness / 2); i <= (thickness / 2); ++i) {
        const int16_t ox = static_cast<int16_t>(nx * i);
        const int16_t oy = static_cast<int16_t>(ny * i);
        gfx.drawLine(x + ox, y + oy, x2 + ox, y2 + oy, node.color565);
      }
      continue;
    }

    if (node.type == dsl::NodeType::kIcon) {
      const String rawPath = node.path.isEmpty() ? node.text : node.path;
      const String iconPath = bindTemplate(rawPath);
      if (iconPath.isEmpty()) {
        continue;
      }
      const IconCacheEntry* icon = loadIcon(iconPath, node.w, node.h);
      if (!icon) {
        continue;
      }
      const bool swap = gfx.getSwapBytes();
      gfx.setSwapBytes(true);
      gfx.pushImage(x, y, icon->w, icon->h, icon->pixels.data());
      gfx.setSwapBytes(swap);
      continue;
    }

    if (node.type == dsl::NodeType::kMoonPhase) {
      float phase = 0.0f;
      bool havePhase = false;
      if (!node.key.isEmpty()) {
        havePhase = getNumeric(node.key, phase);
      }
      if (!havePhase) {
        havePhase = computeMoonPhaseFraction(phase);
      }
      if (!havePhase) {
        continue;
      }

      const int16_t r = node.radius > 0 ? node.radius : (node.w > 0 ? node.w / 2 : 8);
      if (r <= 0) {
        continue;
      }

      const uint16_t bg = node.bg565 == TFT_BLACK ? TFT_BLACK : node.bg565;
      gfx.fillCircle(x, y, r, bg);

      float threshold = 0.0f;
      bool waxing = phase <= 0.5f;
      if (waxing) {
        threshold = r * (1.0f - 2.0f * phase);
      } else {
        threshold = -r * (2.0f * phase - 1.0f);
      }

      for (int16_t dy = -r; dy <= r; ++dy) {
        for (int16_t dx = -r; dx <= r; ++dx) {
          if (dx * dx + dy * dy > r * r) {
            continue;
          }
          const bool lit = waxing ? (dx > threshold) : (dx < threshold);
          if (lit) {
            gfx.drawPixel(x + dx, y + dy, node.color565);
          }
        }
      }

      if (node.thickness > 0) {
        gfx.drawCircle(x, y, r, node.color565);
      }
      continue;
    }
  }
  };

  if (!dslLoaded_) {
    drawPanel(tft, String("DSL"));
    const int16_t cx = config_.x + config_.w - 6;
    const int16_t cy = config_.y + 6;
    tft.fillCircle(cx, cy, 2, TFT_RED);
    return;
  }

  if (useSprite_) {
    if (sprite_ == nullptr) {
      sprite_ = new TFT_eSprite(&tft);
      sprite_->setColorDepth(16);
      spriteReady_ = (sprite_->createSprite(config_.w, config_.h) != nullptr);
    }
    if (spriteReady_) {
      drawPanelTo(*sprite_);
      renderNodes(*sprite_, 0, 0);
      sprite_->pushSprite(config_.x, config_.y);
    } else {
      drawPanel(tft, dslLoaded_ ? dsl_.title : String("DSL"));
      renderNodes(tft, config_.x, config_.y);
    }
  } else {
    drawPanel(tft, dslLoaded_ ? dsl_.title : String("DSL"));
    renderNodes(tft, config_.x, config_.y);
  }

  const int16_t cx = config_.x + config_.w - 6;
  const int16_t cy = config_.y + 6;
  const uint16_t color = (status_ == "ok") ? TFT_GREEN : TFT_RED;
  tft.fillCircle(cx, cy, 2, color);
}
