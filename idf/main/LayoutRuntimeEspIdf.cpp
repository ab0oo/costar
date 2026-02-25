#include "LayoutRuntimeEspIdf.h"

#include "DisplaySpiEspIdf.h"
#include "DslWidgetRuntimeEspIdf.h"

#include "esp_log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace {
constexpr const char* kTag = "layout-runtime";
constexpr uint16_t kBg = 0x0000;
constexpr uint16_t kBorder = 0xFFFF;
constexpr uint16_t kWeather = 0x7BEF;
constexpr uint16_t kForecast = 0x07FF;
constexpr uint16_t kClock = 0xFD20;
constexpr uint16_t kGeneric = 0x4208;

struct Region {
  std::string id;
  std::string widget;
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  std::string type;
  std::string source;
};

std::vector<Region> sRegions;
bool sActive = false;
bool sDrawn = false;
uint32_t sLastPulseMs = 0;
bool sPulseOn = false;

size_t skipWs(const std::string& s, size_t i) {
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) {
    ++i;
  }
  return i;
}

bool parseIntAfterColon(const std::string& block, size_t keyPos, int& out) {
  size_t i = block.find(':', keyPos);
  if (i == std::string::npos) {
    return false;
  }
  i = skipWs(block, i + 1);
  if (i >= block.size()) {
    return false;
  }

  bool neg = false;
  if (block[i] == '-') {
    neg = true;
    ++i;
  }
  if (i >= block.size() || std::isdigit(static_cast<unsigned char>(block[i])) == 0) {
    return false;
  }
  int value = 0;
  while (i < block.size() && std::isdigit(static_cast<unsigned char>(block[i])) != 0) {
    value = value * 10 + (block[i] - '0');
    ++i;
  }
  out = neg ? -value : value;
  return true;
}

bool findIntField(const std::string& block, const char* key, int& out) {
  const std::string token = std::string("\"") + key + "\"";
  const size_t keyPos = block.find(token);
  if (keyPos == std::string::npos) {
    return false;
  }
  return parseIntAfterColon(block, keyPos, out);
}

std::string findStringField(const std::string& block, const char* key) {
  const std::string token = std::string("\"") + key + "\"";
  const size_t keyPos = block.find(token);
  if (keyPos == std::string::npos) {
    return {};
  }
  size_t i = block.find(':', keyPos);
  if (i == std::string::npos) {
    return {};
  }
  i = skipWs(block, i + 1);
  if (i >= block.size() || block[i] != '"') {
    return {};
  }
  ++i;
  const size_t end = block.find('"', i);
  if (end == std::string::npos) {
    return {};
  }
  return block.substr(i, end - i);
}

bool extractRegionObjects(const std::string& json, std::vector<std::string>& out) {
  out.clear();
  const size_t regionsPos = json.find("\"regions\"");
  if (regionsPos == std::string::npos) {
    return false;
  }
  size_t arrStart = json.find('[', regionsPos);
  if (arrStart == std::string::npos) {
    return false;
  }
  size_t i = arrStart + 1;
  int arrDepth = 1;
  int objDepth = 0;
  size_t objStart = std::string::npos;

  while (i < json.size() && arrDepth > 0) {
    const char c = json[i];
    if (c == '[') {
      ++arrDepth;
    } else if (c == ']') {
      --arrDepth;
      if (arrDepth == 0) {
        break;
      }
    } else if (c == '{') {
      if (arrDepth == 1 && objDepth == 0) {
        objStart = i;
      }
      ++objDepth;
    } else if (c == '}') {
      if (objDepth > 0) {
        --objDepth;
        if (arrDepth == 1 && objDepth == 0 && objStart != std::string::npos && i > objStart) {
          out.push_back(json.substr(objStart, i - objStart + 1));
          objStart = std::string::npos;
        }
      }
    }
    ++i;
  }
  return !out.empty();
}

std::string readFile(const char* path) {
  if (path == nullptr || *path == '\0') {
    return {};
  }
  std::FILE* fp = std::fopen(path, "rb");
  if (fp == nullptr) {
    return {};
  }
  if (std::fseek(fp, 0, SEEK_END) != 0) {
    std::fclose(fp);
    return {};
  }
  const long len = std::ftell(fp);
  if (len <= 0) {
    std::fclose(fp);
    return {};
  }
  if (std::fseek(fp, 0, SEEK_SET) != 0) {
    std::fclose(fp);
    return {};
  }
  std::string out(static_cast<size_t>(len), '\0');
  const size_t got = std::fread(out.data(), 1, out.size(), fp);
  std::fclose(fp);
  if (got != out.size()) {
    return {};
  }
  return out;
}

bool parseU16Checked(int value, uint16_t& out) {
  if (value < 0 || value > 65535) {
    return false;
  }
  out = static_cast<uint16_t>(value);
  return true;
}

uint16_t colorForRegion(const Region& r) {
  if (r.widget == "weather-now" || r.source.find("weather_now") != std::string::npos) {
    return kWeather;
  }
  if (r.source.find("forecast") != std::string::npos) {
    return kForecast;
  }
  if (r.source.find("clock") != std::string::npos) {
    return kClock;
  }
  if (r.type.find("dsl") != std::string::npos) {
    return kGeneric;
  }
  return kGeneric;
}

void drawRegionFrame(const Region& r, uint16_t color) {
  const uint16_t x2 = static_cast<uint16_t>(r.x + r.w - 1);
  const uint16_t y2 = static_cast<uint16_t>(r.y + r.h - 1);

  (void)display_spi::fillRect(r.x, r.y, r.w, r.h, color);
  // 1px border
  (void)display_spi::fillRect(r.x, r.y, r.w, 1, kBorder);
  (void)display_spi::fillRect(r.x, y2, r.w, 1, kBorder);
  (void)display_spi::fillRect(r.x, r.y, 1, r.h, kBorder);
  (void)display_spi::fillRect(x2, r.y, 1, r.h, kBorder);
}

void drawScene() {
  (void)display_spi::clear(kBg);
  for (const Region& r : sRegions) {
    drawRegionFrame(r, colorForRegion(r));
  }
  sDrawn = true;
}
}  // namespace

namespace layout_runtime {

bool begin(const char* layoutPath) {
  sRegions.clear();
  sActive = false;
  sDrawn = false;
  sLastPulseMs = 0;
  sPulseOn = false;

  const std::string json = readFile(layoutPath);
  if (json.empty()) {
    ESP_LOGE(kTag, "layout read failed path=%s", layoutPath != nullptr ? layoutPath : "(null)");
    return false;
  }

  std::vector<std::string> regionObjects;
  if (!extractRegionObjects(json, regionObjects)) {
    ESP_LOGE(kTag, "layout parse failed regions[]");
    return false;
  }

  const uint16_t screenW = display_spi::width();
  const uint16_t screenH = display_spi::height();

  for (const std::string& item : regionObjects) {
    Region r = {};
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    if (!findIntField(item, "x", x) || !findIntField(item, "y", y) || !findIntField(item, "w", w) ||
        !findIntField(item, "h", h) || !parseU16Checked(x, r.x) || !parseU16Checked(y, r.y) ||
        !parseU16Checked(w, r.w) || !parseU16Checked(h, r.h) || r.w == 0 || r.h == 0) {
      continue;
    }
    if (r.x >= screenW || r.y >= screenH) {
      continue;
    }
    if (r.w > static_cast<uint16_t>(screenW - r.x)) {
      r.w = static_cast<uint16_t>(screenW - r.x);
    }
    if (r.h > static_cast<uint16_t>(screenH - r.y)) {
      r.h = static_cast<uint16_t>(screenH - r.y);
    }

    r.id = findStringField(item, "id");
    r.widget = findStringField(item, "widget");
    r.type = findStringField(item, "type");
    r.source = findStringField(item, "source");
    sRegions.push_back(std::move(r));
  }

  if (sRegions.empty()) {
    ESP_LOGW(kTag, "no valid regions in layout path=%s", layoutPath != nullptr ? layoutPath : "(null)");
    return false;
  }

  ESP_LOGI(kTag, "loaded regions=%u path=%s", static_cast<unsigned>(sRegions.size()),
           layoutPath != nullptr ? layoutPath : "(null)");
  for (size_t i = 0; i < sRegions.size(); ++i) {
    const Region& r = sRegions[i];
    ESP_LOGI(kTag, "region[%u] id=%s widget=%s type=%s src=%s rect=%u,%u %ux%u",
             static_cast<unsigned>(i), r.id.c_str(), r.widget.c_str(), r.type.c_str(),
             r.source.c_str(), r.x, r.y, r.w, r.h);
  }

  drawScene();
  for (const Region& r : sRegions) {
    if (r.widget.empty()) {
      continue;
    }
    std::string dslName = r.widget;
    std::replace(dslName.begin(), dslName.end(), '-', '_');
    const std::string dslPath = "/littlefs/dsl_active/" + dslName + ".json";
    if (dsl_widget_runtime::begin(r.widget.c_str(), dslPath.c_str(), r.x, r.y, r.w, r.h)) {
      break;  // first active DSL widget for this migration step
    }
  }
  sActive = true;
  return true;
}

void tick(uint32_t nowMs) {
  if (!sActive) {
    return;
  }
  if (!sDrawn) {
    drawScene();
  }

  // Lightweight heartbeat: pulse border of first region once/sec so runtime
  // phase is visually and temporally alive while full widget renderer is ported.
  if (sRegions.empty()) {
    return;
  }
  if (nowMs - sLastPulseMs < 1000U) {
    return;
  }
  sLastPulseMs = nowMs;
  sPulseOn = !sPulseOn;
  dsl_widget_runtime::tick(nowMs);

  const Region& r = sRegions.front();
  const uint16_t pulse = sPulseOn ? 0xFFE0 : kBorder;
  (void)display_spi::fillRect(r.x, r.y, r.w, 1, pulse);
  (void)display_spi::fillRect(r.x, static_cast<uint16_t>(r.y + r.h - 1), r.w, 1, pulse);
  (void)display_spi::fillRect(r.x, r.y, 1, r.h, pulse);
  (void)display_spi::fillRect(static_cast<uint16_t>(r.x + r.w - 1), r.y, 1, r.h, pulse);
}

bool isActive() { return sActive; }

}  // namespace layout_runtime
