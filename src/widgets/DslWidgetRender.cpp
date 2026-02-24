#include "widgets/DslWidget.h"

#include <algorithm>
#include <math.h>
#include <map>
#include <string.h>
#include <utility>
#include <vector>

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "platform/Fs.h"
#include "platform/Net.h"

namespace {
struct IconCacheEntry {
  String key;
  int16_t w = 0;
  int16_t h = 0;
  std::vector<uint16_t> pixels;
};

constexpr size_t kMaxIconCache = 12;
std::vector<IconCacheEntry> sIconCache;
std::map<String, uint32_t> sRemoteIconRetryAfterMs;
constexpr uint32_t kRemoteIconRetryMs = 30000U;
constexpr uint32_t kRemoteIconIoTimeoutMs = 10000U;
constexpr char kIconCacheDir[] = "/icon_cache";

void pruneRemoteIconRetryMap(uint32_t nowMs) {
  for (auto it = sRemoteIconRetryAfterMs.begin(); it != sRemoteIconRetryAfterMs.end();) {
    if (static_cast<int32_t>(nowMs - it->second) >= 0) {
      it = sRemoteIconRetryAfterMs.erase(it);
    } else {
      ++it;
    }
  }
}

bool isRemoteIconPath(const String& path) {
  return path.startsWith("http://") || path.startsWith("https://");
}

bool hasEmptyIconQuery(const String& path) {
  const int iconPos = path.indexOf("icon=");
  if (iconPos < 0) {
    return false;
  }
  const int valStart = iconPos + 5;
  if (valStart >= path.length()) {
    return true;
  }
  const char next = path[valStart];
  return next == '&' || next == '#';
}

uint32_t fnv1a32(const String& text) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < text.length(); ++i) {
    hash ^= static_cast<uint8_t>(text[i]);
    hash *= 16777619u;
  }
  return hash;
}

String remoteIconCachePath(const String& url, int16_t w, int16_t h) {
  const uint32_t hash = fnv1a32(url);
  char hashHex[9];
  snprintf(hashHex, sizeof(hashHex), "%08lx", static_cast<unsigned long>(hash));
  return String(kIconCacheDir) + "/" + String(hashHex) + "_" + String(w) + "x" + String(h) +
         ".raw";
}

bool ensureIconCacheDir() {
  if (platform::fs::exists(kIconCacheDir)) {
    return true;
  }
  return platform::fs::mkdir(kIconCacheDir);
}

const IconCacheEntry* findIcon(const String& key) {
  for (const auto& entry : sIconCache) {
    if (entry.key == key) {
      return &entry;
    }
  }
  return nullptr;
}

bool loadIconPixelsFromFile(const String& filePath, const String& cacheKey,
                            int16_t w, int16_t h) {
  platform::fs::File f = platform::fs::open(filePath, FILE_READ);
  if (!f || f.isDirectory()) {
    return false;
  }

  const size_t expected = static_cast<size_t>(w) * static_cast<size_t>(h) * 2U;
  if (f.size() < expected) {
    f.close();
    return false;
  }

  IconCacheEntry entry;
  entry.key = cacheKey;
  entry.w = w;
  entry.h = h;
  entry.pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
  const size_t read = f.read(reinterpret_cast<uint8_t*>(entry.pixels.data()), expected);
  f.close();
  if (read != expected) {
    return false;
  }

  if (sIconCache.size() >= kMaxIconCache) {
    sIconCache.erase(sIconCache.begin());
  }
  sIconCache.push_back(std::move(entry));
  return true;
}

uint32_t parseRetryAfterMs(const String& retryAfter) {
  if (retryAfter.isEmpty()) {
    return kRemoteIconRetryMs;
  }
  bool hasDigit = false;
  for (size_t i = 0; i < retryAfter.length(); ++i) {
    const char c = retryAfter[i];
    if (c >= '0' && c <= '9') {
      hasDigit = true;
      continue;
    }
    if (c == ' ' || c == '\t') {
      continue;
    }
    return kRemoteIconRetryMs;
  }
  if (!hasDigit) {
    return kRemoteIconRetryMs;
  }
  const long seconds = retryAfter.toInt();
  if (seconds <= 0) {
    return kRemoteIconRetryMs;
  }
  const uint32_t ms = static_cast<uint32_t>(seconds) * 1000U;
  if (ms < 1000U) {
    return 1000U;
  }
  if (ms > 300000U) {
    return 300000U;
  }
  return ms;
}

bool fetchRemoteIconToFile(const String& url, const String& outPath, int16_t w, int16_t h) {
  if (!platform::net::isConnected()) {
    return false;
  }

  const uint32_t nowMs = millis();
  pruneRemoteIconRetryMap(nowMs);
  auto retryIt = sRemoteIconRetryAfterMs.find(url);
  if (retryIt != sRemoteIconRetryAfterMs.end() &&
      static_cast<int32_t>(nowMs - retryIt->second) < 0) {
    return false;
  }

  const size_t expected = static_cast<size_t>(w) * static_cast<size_t>(h) * 2U;
  if (expected == 0) {
    return false;
  }

  HTTPClient http;
  WiFiClientSecure secureClient;
  if (url.startsWith("https://")) {
    secureClient.setInsecure();
    if (!http.begin(secureClient, url)) {
      sRemoteIconRetryAfterMs[url] = nowMs + kRemoteIconRetryMs;
      return false;
    }
  } else {
    if (!http.begin(url)) {
      sRemoteIconRetryAfterMs[url] = nowMs + kRemoteIconRetryMs;
      return false;
    }
  }
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setRedirectLimit(4);
  http.setConnectTimeout(4000);
  http.setTimeout(7000);
  http.useHTTP10(true);
  http.setReuse(false);
  const char* headerKeys[] = {"Retry-After", "Content-Length"};
  http.collectHeaders(headerKeys, 2);
  http.addHeader("Accept", "application/octet-stream");
  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    const uint32_t retryMs =
        (status == 429 || status == 503) ? parseRetryAfterMs(http.header("Retry-After"))
                                         : kRemoteIconRetryMs;
    sRemoteIconRetryAfterMs[url] = nowMs + retryMs;
    http.end();
    return false;
  }

  const int contentLength = http.getSize();
  if (contentLength > 0 && static_cast<size_t>(contentLength) < expected) {
    sRemoteIconRetryAfterMs[url] = nowMs + kRemoteIconRetryMs;
    http.end();
    return false;
  }

  const String tempPath = outPath + ".tmp";
  platform::fs::remove(tempPath);
  platform::fs::File out = platform::fs::open(tempPath, FILE_WRITE);
  if (!out || out.isDirectory()) {
    sRemoteIconRetryAfterMs[url] = nowMs + kRemoteIconRetryMs;
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t total = 0;
  uint32_t ioStartMs = millis();
  uint32_t lastByteMs = ioStartMs;
  while ((http.connected() || (stream && stream->available() > 0)) &&
         (millis() - ioStartMs) < kRemoteIconIoTimeoutMs && total < expected) {
    if (stream == nullptr) {
      break;
    }
    const size_t available = stream->available();
    if (available > 0) {
      uint8_t buf[128];
      size_t toRead = available > sizeof(buf) ? sizeof(buf) : available;
      if (total + toRead > expected) {
        toRead = expected - total;
      }
      const int readCount = stream->readBytes(reinterpret_cast<char*>(buf), toRead);
      if (readCount > 0) {
        const size_t written = out.write(buf, static_cast<size_t>(readCount));
        if (written != static_cast<size_t>(readCount)) {
          out.close();
          http.end();
          platform::fs::remove(tempPath);
          sRemoteIconRetryAfterMs[url] = nowMs + kRemoteIconRetryMs;
          return false;
        }
        total += written;
        lastByteMs = millis();
      }
    } else {
      if ((millis() - lastByteMs) > 500) {
        break;
      }
      delay(5);
    }
  }

  out.close();
  http.end();

  if (total != expected) {
    platform::fs::remove(tempPath);
    sRemoteIconRetryAfterMs[url] = nowMs + kRemoteIconRetryMs;
    return false;
  }

  platform::fs::remove(outPath);
  if (!platform::fs::rename(tempPath, outPath)) {
    platform::fs::remove(tempPath);
    sRemoteIconRetryAfterMs[url] = nowMs + kRemoteIconRetryMs;
    return false;
  }

  sRemoteIconRetryAfterMs.erase(url);
  return true;
}

const IconCacheEntry* loadIcon(const String& path, int16_t w, int16_t h) {
  if (path.isEmpty() || w <= 0 || h <= 0) {
    return nullptr;
  }
  const String key = path + "#" + String(w) + "x" + String(h);
  if (const IconCacheEntry* cached = findIcon(key)) {
    return cached;
  }
  if (!isRemoteIconPath(path)) {
    if (loadIconPixelsFromFile(path, key, w, h)) {
      return &sIconCache.back();
    }
    return nullptr;
  }
  if (hasEmptyIconQuery(path)) {
    return nullptr;
  }
  if (!ensureIconCacheDir()) {
    return nullptr;
  }

  const String cachePath = remoteIconCachePath(path, w, h);
  if (loadIconPixelsFromFile(cachePath, key, w, h)) {
    return &sIconCache.back();
  }
  if (!fetchRemoteIconToFile(path, cachePath, w, h)) {
    return nullptr;
  }
  if (!loadIconPixelsFromFile(cachePath, key, w, h)) {
    return nullptr;
  }
  return &sIconCache.back();
}

bool isCenterDatum(uint8_t datum) {
  return datum == TC_DATUM || datum == MC_DATUM || datum == BC_DATUM ||
         datum == C_BASELINE;
}

bool isRightDatum(uint8_t datum) {
  return datum == TR_DATUM || datum == MR_DATUM || datum == BR_DATUM ||
         datum == R_BASELINE;
}

bool isMiddleDatum(uint8_t datum) {
  return datum == ML_DATUM || datum == MC_DATUM || datum == MR_DATUM;
}

bool isBottomDatum(uint8_t datum) {
  return datum == BL_DATUM || datum == BC_DATUM || datum == BR_DATUM;
}

uint8_t topLineDatum(uint8_t datum) {
  if (isCenterDatum(datum)) {
    return TC_DATUM;
  }
  if (isRightDatum(datum)) {
    return TR_DATUM;
  }
  return TL_DATUM;
}

uint8_t safeFontId(uint8_t font) {
  // TFT_eSPI built-in bitmap fonts are 1..8.
  if (font < 1 || font > 8) {
    return 2;
  }
  return font;
}

template <typename Gfx>
int16_t textWidthPx(Gfx& gfx, const String& text, uint8_t font) {
  if (text.length() == 0) {
    return 0;
  }
  String safe = text;
  if (safe.length() > 160) {
    safe = safe.substring(0, 160);
  }
  safe.replace('\r', ' ');
  safe.replace('\n', ' ');
  safe.trim();
  if (safe.length() == 0) {
    return 0;
  }
  char buf[161];
  const size_t n = safe.length() > 160 ? 160 : safe.length();
  memcpy(buf, safe.c_str(), n);
  buf[n] = '\0';
  return static_cast<int16_t>(gfx.textWidth(buf, safeFontId(font)));
}

template <typename Gfx>
void safeDrawString(Gfx& gfx, const String& text, int16_t x, int16_t y, uint8_t font) {
  if (text.length() == 0) {
    return;
  }
  String safe = text;
  if (safe.length() > 160) {
    safe = safe.substring(0, 160);
  }
  safe.replace('\r', ' ');
  safe.replace('\n', ' ');
  safe.trim();
  if (safe.length() == 0) {
    return;
  }
  char buf[161];
  const size_t n = safe.length() > 160 ? 160 : safe.length();
  memcpy(buf, safe.c_str(), n);
  buf[n] = '\0';
  gfx.drawString(buf, x, y, safeFontId(font));
}

template <typename Gfx>
String ellipsizeToWidth(Gfx& gfx, const String& text, uint8_t font, int16_t maxWidth) {
  if (maxWidth <= 0) {
    return String();
  }
  if (textWidthPx(gfx, text, font) <= maxWidth) {
    return text;
  }

  const String dots = "...";
  if (textWidthPx(gfx, dots, font) > maxWidth) {
    for (int i = dots.length(); i > 0; --i) {
      const String candidate = dots.substring(0, i);
      if (textWidthPx(gfx, candidate, font) <= maxWidth) {
        return candidate;
      }
    }
    return String();
  }

  for (int len = text.length(); len > 0; --len) {
    const String candidate = text.substring(0, len) + dots;
    if (textWidthPx(gfx, candidate, font) <= maxWidth) {
      return candidate;
    }
  }
  return dots;
}

template <typename Gfx>
std::vector<String> wrapLabelLines(Gfx& gfx, const String& text, uint8_t font,
                                   int16_t maxWidth) {
  std::vector<String> lines;
  if (maxWidth <= 0) {
    lines.push_back(text);
    return lines;
  }

  String currentLine;
  String currentWord;

  auto placeWord = [&](const String& word) {
    if (word.isEmpty()) {
      return;
    }

    auto placeLongWord = [&](const String& longWord) {
      int start = 0;
      while (start < longWord.length()) {
        int bestEnd = start;
        for (int end = start + 1; end <= longWord.length(); ++end) {
          const String piece = longWord.substring(start, end);
          if (textWidthPx(gfx, piece, font) <= maxWidth) {
            bestEnd = end;
            continue;
          }
          break;
        }
        if (bestEnd == start) {
          bestEnd = start + 1;
        }

        const String piece = longWord.substring(start, bestEnd);
        start = bestEnd;
        if (start < longWord.length()) {
          lines.push_back(piece);
        } else {
          currentLine = piece;
        }
      }
    };

    if (currentLine.isEmpty()) {
      if (textWidthPx(gfx, word, font) <= maxWidth) {
        currentLine = word;
      } else {
        placeLongWord(word);
      }
      return;
    }

    const String candidate = currentLine + " " + word;
    if (textWidthPx(gfx, candidate, font) <= maxWidth) {
      currentLine = candidate;
      return;
    }

    lines.push_back(currentLine);
    currentLine = "";
    if (textWidthPx(gfx, word, font) <= maxWidth) {
      currentLine = word;
    } else {
      placeLongWord(word);
    }
  };

  auto flushWord = [&]() {
    placeWord(currentWord);
    currentWord = "";
  };

  for (int i = 0; i < text.length(); ++i) {
    const char c = text[i];
    if (c == '\n') {
      flushWord();
      lines.push_back(currentLine);
      currentLine = "";
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      flushWord();
      continue;
    }
    currentWord += c;
  }

  flushWord();
  if (!currentLine.isEmpty() || lines.empty()) {
    lines.push_back(currentLine);
  }
  return lines;
}
}  // namespace

void DslWidget::render(TFT_eSPI& tft) {
  auto drawPanelTo = [&](auto& gfx) {
    gfx.fillRect(0, 0, config_.w, config_.h, TFT_BLACK);
    if (config_.drawBorder) {
      gfx.drawRect(0, 0, config_.w, config_.h, TFT_DARKGREY);
    }
  };

  auto renderNodes = [&](auto& gfx, int16_t baseX, int16_t baseY, int16_t clipW, int16_t clipH) {
    for (const auto& node : dsl_.nodes) {
      const int16_t x = baseX + node.x;
      const int16_t y = baseY + node.y;

      if (node.type == dsl::NodeType::kLabel) {
        if ((node.font < 1 || node.font > 8) && dsl_.debug) {
          platform::logf("[%s] [%s] invalid font id=%u; using 2\n", widgetName().c_str(),
                        logTimestamp().c_str(), static_cast<unsigned>(node.font));
        }
        const uint8_t font = safeFontId(node.font);
        gfx.setTextColor(node.color565, TFT_BLACK);
        String labelText = bindTemplate(node.text);
        if (!node.path.isEmpty()) {
          String valueText;
          auto it = pathValues_.find(node.path);
          if (it != pathValues_.end()) {
            valueText = it->second;
          }
          if (node.text.isEmpty()) {
            labelText = valueText;
          } else {
            labelText.replace("{{value}}", valueText);
          }
        }
        if (!node.wrap || node.w <= 0) {
          gfx.setTextDatum(node.datum);
          safeDrawString(gfx, labelText, x, y, font);
          continue;
        }

        int16_t lineHeight = node.lineHeight > 0 ? node.lineHeight : gfx.fontHeight(font);
        if (lineHeight <= 0) {
          lineHeight = 10;
        }

        int16_t maxLines = node.maxLines > 0 ? node.maxLines : 0;
        if (node.h > 0) {
          const int16_t fromHeight = node.h / lineHeight;
          if (fromHeight > 0) {
            maxLines = (maxLines > 0) ? std::min(maxLines, fromHeight) : fromHeight;
          }
        }

        std::vector<String> lines = wrapLabelLines(gfx, labelText, font, node.w);
        bool truncated = false;
        if (maxLines > 0 && lines.size() > static_cast<size_t>(maxLines)) {
          lines.resize(static_cast<size_t>(maxLines));
          truncated = true;
        }
        if (truncated && !lines.empty() && node.overflow == dsl::OverflowMode::kEllipsis) {
          lines.back() = ellipsizeToWidth(gfx, lines.back(), font, node.w);
        }

        const int16_t blockHeight = static_cast<int16_t>(lines.size()) * lineHeight;
        int16_t startY = y;
        if (isMiddleDatum(node.datum)) {
          startY = y - (blockHeight / 2);
        } else if (isBottomDatum(node.datum)) {
          startY = y - blockHeight;
        }

        gfx.setTextDatum(topLineDatum(node.datum));
        for (size_t i = 0; i < lines.size(); ++i) {
          if (lines[i].length() == 0) {
            continue;
          }
          const int16_t lineY = startY + static_cast<int16_t>(i) * lineHeight;
          safeDrawString(gfx, lines[i], x, lineY, font);
        }
        continue;
      }

      if (node.type == dsl::NodeType::kValueBox) {
        if ((node.font < 1 || node.font > 8) && dsl_.debug) {
          platform::logf("[%s] [%s] invalid font id=%u; using 2\n", widgetName().c_str(),
                        logTimestamp().c_str(), static_cast<unsigned>(node.font));
        }
        const uint8_t font = safeFontId(node.font);
        gfx.fillRect(x, y, node.w, node.h, node.bg565);
        gfx.drawRect(x, y, node.w, node.h, node.color565);
        gfx.setTextColor(node.color565, node.bg565);
        gfx.setTextDatum(TL_DATUM);
        if (!node.text.isEmpty()) {
          safeDrawString(gfx, bindTemplate(node.text), x + 4, y + 4, 1);
        }
        const String value = node.key.isEmpty() ? String() : values_[node.key];
        safeDrawString(gfx, value, x + 4, y + 16, font);
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
        safeDrawString(gfx, String(value, 1), x + node.w / 2, y + node.h / 2, 1);
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
	      if (icon->w <= 0 || icon->h <= 0) {
	        continue;
	      }
	      const size_t needPixels = static_cast<size_t>(icon->w) * static_cast<size_t>(icon->h);
	      if (icon->pixels.empty() || icon->pixels.size() < needPixels ||
	          icon->pixels.data() == nullptr) {
	        continue;
	      }
	      if (x < baseX || y < baseY || (x + icon->w) > (baseX + clipW) ||
	          (y + icon->h) > (baseY + clipH)) {
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
      renderNodes(*sprite_, 0, 0, config_.w, config_.h);
      sprite_->pushSprite(config_.x, config_.y);
    } else {
      drawPanel(tft, dslLoaded_ ? dsl_.title : String("DSL"));
      renderNodes(tft, config_.x, config_.y, config_.w, config_.h);
    }
  } else {
    drawPanel(tft, dslLoaded_ ? dsl_.title : String("DSL"));
    renderNodes(tft, config_.x, config_.y, config_.w, config_.h);
  }

  const int16_t cx = config_.x + config_.w - 6;
  const int16_t cy = config_.y + 6;
  const uint16_t color = (status_ == "ok") ? TFT_GREEN : TFT_RED;
  tft.fillCircle(cx, cy, 2, color);
}
