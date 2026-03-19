#include "ConfigScreenEspIdf.h"

#include "AppConfig.h"
#include "DisplaySpiEspIdf.h"
#include "Font5x7Classic.h"

#include "esp_log.h"

#include <cstdio>
#include <cstdint>
#include <string>

namespace {
constexpr const char* kTag = "config";

struct Rect { uint16_t x, y, w, h; };

struct UiLayout {
  bool valid = false;
  Rect toggleClock;
  Rect toggleTemp;
  Rect toggleDist;
};

UiLayout sUi;

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

void drawChar5x7(int x, int y, char c, uint16_t fg, uint16_t bg, int scale) {
  if (c < 0x20 || c > 0x7E) c = '?';
  const size_t idx = static_cast<size_t>(static_cast<uint8_t>(c)) * 5U;
  for (int col = 0; col < 5; ++col) {
    const uint8_t line = font[idx + static_cast<size_t>(col)];
    for (int row = 0; row < 8; ++row) {
      const bool on = ((line >> row) & 0x01U) != 0U;
      (void)display_spi::fillRect(
          static_cast<uint16_t>(x + col * scale), static_cast<uint16_t>(y + row * scale),
          static_cast<uint16_t>(scale), static_cast<uint16_t>(scale), on ? fg : bg);
    }
  }
}

void drawText(int x, int y, const char* text, uint16_t fg, uint16_t bg, int scale) {
  if (!text) return;
  int penX = x;
  for (const char* p = text; *p; ++p) {
    drawChar5x7(penX, y, *p, fg, bg, scale);
    penX += scale * 6;
  }
}

bool contains(const Rect& r, uint16_t x, uint16_t y) {
  if (r.w == 0 || r.h == 0) return false;
  return x >= r.x && x < static_cast<uint16_t>(r.x + r.w) &&
         y >= r.y && y < static_cast<uint16_t>(r.y + r.h);
}

void drawButton(const Rect& r, const char* label, uint16_t bg, uint16_t fg, int scale) {
  if (r.w == 0 || r.h == 0) return;
  (void)display_spi::fillRect(r.x, r.y, r.w, r.h, bg);
  drawText(r.x + 8, r.y + (r.h > 20 ? 6 : 4), label, fg, bg, scale);
}

}  // namespace

namespace config_screen {

void show(const ViewState& state) {
  const uint16_t w = AppConfig::kScreenWidth;
  const uint16_t h = AppConfig::kScreenHeight;
  if (w == 0 || h == 0) { sUi.valid = false; return; }

  const uint16_t cBg     = rgb565(9,  12, 22);
  const uint16_t cHeader = rgb565(18, 28, 48);
  const uint16_t cCard   = rgb565(22, 35, 58);
  const uint16_t cBtn    = rgb565(34, 66, 112);
  const uint16_t cGood   = rgb565(30, 140, 60);
  const uint16_t cWarn   = rgb565(170, 110, 10);
  const uint16_t cText   = rgb565(210, 225, 245);
  const uint16_t cSubtle = rgb565(130, 160, 205);
  const uint16_t cValue  = rgb565(255, 228, 110);

  (void)display_spi::clear(cBg);
  (void)display_spi::fillRect(0, 0, w, 34, cHeader);
  drawText(10, 10, "LOCALE CONFIG", cText, cHeader, 2);

  // WiFi status line
  const uint16_t wifiColor = state.wifiConnected ? cGood : cWarn;
  drawText(14, 42, "WIFI:", cSubtle, cBg, 1);
  drawText(50, 42, state.wifiConnected ? "CONNECTED" : "OFFLINE", wifiColor, cBg, 1);

  // Locale card
  const uint16_t cardY = 60;
  const uint16_t cardH = static_cast<uint16_t>(h - cardY - 10);
  (void)display_spi::fillRect(8, cardY, static_cast<uint16_t>(w - 16), cardH, cCard);
  drawText(16, static_cast<int>(cardY) + 8, "LOCALE", cText, cCard, 2);

  // Three toggle buttons
  const uint16_t btnY  = static_cast<uint16_t>(cardY + 36);
  const uint16_t gap   = 6;
  const uint16_t btnW  = static_cast<uint16_t>((w - 16 - 2 * gap) / 3);
  sUi = {};
  sUi.valid = true;
  sUi.toggleClock = {8,                                          btnY, btnW, 36};
  sUi.toggleTemp  = {static_cast<uint16_t>(8 + btnW + gap),     btnY, btnW, 36};
  sUi.toggleDist  = {static_cast<uint16_t>(8 + (btnW+gap) * 2), btnY, btnW, 36};

  drawButton(sUi.toggleClock, "TIME",  cBtn, cText, 1);
  drawButton(sUi.toggleTemp,  "TEMP",  cBtn, cText, 1);
  drawButton(sUi.toggleDist,  "DIST",  cBtn, cText, 1);

  drawText(sUi.toggleClock.x + 8, sUi.toggleClock.y + 22,
           state.use24HourClock ? "24H" : "12H", cValue, cBtn, 1);
  drawText(sUi.toggleTemp.x + 8, sUi.toggleTemp.y + 22,
           state.useFahrenheit ? "F" : "C", cValue, cBtn, 1);
  drawText(sUi.toggleDist.x + 8, sUi.toggleDist.y + 22,
           state.useMiles ? "MI" : "KM", cValue, cBtn, 1);

  ESP_LOGI(kTag, "locale config screen drawn w=%u h=%u", w, h);
}

void showWifiStatus(const char* title, const char* subtitle, bool isError) {
  const uint16_t w = AppConfig::kScreenWidth;
  const uint16_t h = AppConfig::kScreenHeight;
  if (w == 0 || h == 0) return;
  sUi.valid = false;

  const uint16_t cBg     = rgb565(8,  10, 18);
  const uint16_t cHeader = rgb565(18, 28, 48);
  const uint16_t cText   = rgb565(210, 225, 245);
  const uint16_t cErr    = rgb565(230, 120, 120);

  (void)display_spi::clear(cBg);
  (void)display_spi::fillRect(0, 0, w, 34, cHeader);
  drawText(10, 10, title ? title : "WIFI", cText, cHeader, 2);
  drawText(14, h / 2, subtitle ? subtitle : "", isError ? cErr : cText, cBg, 1);
}

Action hitTest(uint16_t x, uint16_t y) {
  if (!sUi.valid) return Action::None;
  if (contains(sUi.toggleClock, x, y)) return Action::ToggleClock;
  if (contains(sUi.toggleTemp,  x, y)) return Action::ToggleTemp;
  if (contains(sUi.toggleDist,  x, y)) return Action::ToggleDistance;
  return Action::None;
}

void markTouch(uint16_t x, uint16_t y) {
  const uint16_t w = AppConfig::kScreenWidth;
  const uint16_t h = AppConfig::kScreenHeight;
  if (x >= w || y >= h) return;
  const uint16_t marker = rgb565(255, 210, 40);
  const uint16_t px = x > 1 ? static_cast<uint16_t>(x - 1) : 0;
  const uint16_t py = y > 1 ? static_cast<uint16_t>(y - 1) : 0;
  (void)display_spi::fillRect(px, py, 3, 3, marker);
}

}  // namespace config_screen
