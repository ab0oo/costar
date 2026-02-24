#include "ConfigScreenEspIdf.h"

#include "DisplaySpiEspIdf.h"
#include "AppConfig.h"

#include "esp_log.h"

#include <cstdio>
#include <cstdint>
#include <cstring>

namespace {
constexpr const char* kTag = "config";

struct Rect {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t w = 0;
  uint16_t h = 0;
};

struct UiLayout {
  bool valid = false;
  bool wifiButtons = false;
  Rect retry;
  Rect scan;
  Rect offline;
  Rect toggleClock;
  Rect toggleTemp;
  Rect toggleDist;
};

UiLayout sUi;

struct WifiListLayout {
  bool valid = false;
  Rect back;
  uint16_t startY = 0;
  uint16_t rowH = 0;
  uint16_t count = 0;
};

WifiListLayout sWifiList;

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

uint16_t glyphBits(char c) {
  switch (c) {
    case 'A': return 0b010101111101101;
    case 'B': return 0b110101110101110;
    case 'C': return 0b011100100100011;
    case 'D': return 0b110101101101110;
    case 'E': return 0b111100110100111;
    case 'F': return 0b111100110100100;
    case 'G': return 0b011100101101011;
    case 'H': return 0b101101111101101;
    case 'I': return 0b111010010010111;
    case 'K': return 0b101101110101101;
    case 'L': return 0b100100100100111;
    case 'M': return 0b101111111101101;
    case 'N': return 0b101111111111101;
    case 'O': return 0b010101101101010;
    case 'P': return 0b110101110100100;
    case 'Q': return 0b010101101111011;
    case 'R': return 0b110101110101101;
    case 'S': return 0b011100010001110;
    case 'T': return 0b111010010010010;
    case 'U': return 0b101101101101111;
    case 'V': return 0b101101101101010;
    case 'W': return 0b101101111111101;
    case 'Y': return 0b101101010010010;
    case '-': return 0b000000111000000;
    case ':': return 0b000010000010000;
    case '0': return 0b111101101101111;
    case '1': return 0b010110010010111;
    case '2': return 0b111001111100111;
    case '3': return 0b111001111001111;
    case '4': return 0b101101111001001;
    case '5': return 0b111100111001111;
    case '6': return 0b111100111101111;
    case '7': return 0b111001001001001;
    case '8': return 0b111101111101111;
    case '9': return 0b111101111001111;
    default: return 0;
  }
}

void drawGlyph(int x, int y, char c, uint16_t fg, uint16_t bg, int scale) {
  const uint16_t bits = glyphBits(c);
  for (int row = 0; row < 5; ++row) {
    for (int col = 0; col < 3; ++col) {
      const int bit = 14 - (row * 3 + col);
      const bool on = ((bits >> bit) & 1U) != 0U;
      (void)display_spi::fillRect(static_cast<uint16_t>(x + col * scale),
                                  static_cast<uint16_t>(y + row * scale),
                                  static_cast<uint16_t>(scale), static_cast<uint16_t>(scale),
                                  on ? fg : bg);
    }
  }
}

void drawText(int x, int y, const char* text, uint16_t fg, uint16_t bg, int scale) {
  if (text == nullptr) {
    return;
  }
  int penX = x;
  const size_t len = std::strlen(text);
  for (size_t i = 0; i < len; ++i) {
    char c = text[i];
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - ('a' - 'A'));
    }
    if (c == ' ') {
      penX += scale * 2;
      continue;
    }
    drawGlyph(penX, y, c, fg, bg, scale);
    penX += scale * 4;
  }
}

bool contains(const Rect& r, uint16_t x, uint16_t y) {
  if (r.w == 0 || r.h == 0) {
    return false;
  }
  return x >= r.x && x < static_cast<uint16_t>(r.x + r.w) && y >= r.y &&
         y < static_cast<uint16_t>(r.y + r.h);
}

void drawButton(const Rect& r, const char* label, uint16_t bg, uint16_t fg, int scale) {
  if (r.w == 0 || r.h == 0) {
    return;
  }
  (void)display_spi::fillRect(r.x, r.y, r.w, r.h, bg);
  const int textX = static_cast<int>(r.x) + 8;
  const int textY = static_cast<int>(r.y) + static_cast<int>((r.h > 20 ? 6 : 4));
  drawText(textX, textY, label, fg, bg, scale);
}
}  // namespace

namespace config_screen {

void show(const ViewState& state) {
  const uint16_t w = AppConfig::kScreenWidth;
  const uint16_t h = AppConfig::kScreenHeight;
  if (w == 0 || h == 0) {
    sUi.valid = false;
    return;
  }

  const uint16_t cBg = rgb565(9, 12, 22);
  const uint16_t cHeader = rgb565(18, 28, 48);
  const uint16_t cCard = rgb565(22, 35, 58);
  const uint16_t cBtn = rgb565(34, 66, 112);
  const uint16_t cBtnWarn = rgb565(120, 58, 24);
  const uint16_t cGood = rgb565(30, 140, 60);
  const uint16_t cWarn = rgb565(170, 110, 10);
  const uint16_t cText = rgb565(210, 225, 245);
  const uint16_t cSubtle = rgb565(130, 160, 205);
  const uint16_t cValue = rgb565(255, 228, 110);

  const uint16_t cardY1 = 46;
  const uint16_t cardH = (h - 62) / 2;
  const uint16_t cardY2 = static_cast<uint16_t>(54 + cardH);

  sUi = {};
  sWifiList = {};
  sUi.valid = true;
  sUi.wifiButtons = state.showWifiButtons;
  const uint16_t wifiBtnY = static_cast<uint16_t>(cardY1 + cardH - 28);
  const uint16_t wifiBtnGap = 6;
  const uint16_t wifiBtnW = static_cast<uint16_t>((w - 32 - wifiBtnGap * 2) / 3);
  sUi.retry = {16, wifiBtnY, wifiBtnW, 22};
  sUi.scan = {static_cast<uint16_t>(16 + wifiBtnW + wifiBtnGap), wifiBtnY, wifiBtnW, 22};
  sUi.offline = {static_cast<uint16_t>(16 + (wifiBtnW + wifiBtnGap) * 2), wifiBtnY, wifiBtnW, 22};
  const uint16_t card2InnerY = static_cast<uint16_t>(cardY2 + 34);
  const uint16_t gap = 6;
  const uint16_t btnW = static_cast<uint16_t>((w - 16 - 2 * gap) / 3);
  sUi.toggleClock = {8, card2InnerY, btnW, 36};
  sUi.toggleTemp = {static_cast<uint16_t>(8 + btnW + gap), card2InnerY, btnW, 36};
  sUi.toggleDist = {static_cast<uint16_t>(8 + (btnW + gap) * 2), card2InnerY, btnW, 36};

  (void)display_spi::clear(cBg);
  (void)display_spi::fillRect(0, 0, w, 34, cHeader);
  (void)display_spi::fillRect(8, cardY1, w - 16, cardH, cCard);
  (void)display_spi::fillRect(8, cardY2, w - 16, cardH, cCard);

  drawText(10, 10, "WIFI LOCALE CONFIG", cText, cHeader, 3);

  const uint16_t wifiStateColor =
      state.wifiConnected ? cGood : (state.hasStoredCreds ? cWarn : cSubtle);
  const uint16_t localeStateColor = state.wifiConnected ? cGood : cWarn;

  drawText(16, 58, "WIFI", cText, cCard, 3);
  drawText(16, 80,
           state.wifiConnected ? "CONNECTED" : (state.hasStoredCreds ? "RETRY NEEDED" : "NEEDS SETUP"),
           wifiStateColor, cCard, 2);
  if (state.showWifiButtons) {
    drawButton(sUi.retry, "RETRY", cBtn, cText, 2);
    drawButton(sUi.scan, "SCAN", cBtn, cText, 2);
    drawButton(sUi.offline, "OFFLINE", cBtnWarn, cText, 2);
  }

  drawText(16, static_cast<int>(62 + h / 2), "LOCALE", cText, cCard, 3);
  drawText(16, static_cast<int>(84 + h / 2), state.wifiConnected ? "AUTO READY" : "MANUAL PENDING",
           localeStateColor, cCard, 2);
  drawButton(sUi.toggleClock, "TIME", cBtn, cText, 2);
  drawButton(sUi.toggleTemp, "TEMP", cBtn, cText, 2);
  drawButton(sUi.toggleDist, "DIST", cBtn, cText, 2);

  drawText(static_cast<int>(sUi.toggleClock.x) + 8, static_cast<int>(sUi.toggleClock.y) + 20,
           state.use24HourClock ? "24H" : "12H", cValue, cBtn, 2);
  drawText(static_cast<int>(sUi.toggleTemp.x) + 8, static_cast<int>(sUi.toggleTemp.y) + 20,
           state.useFahrenheit ? "F" : "C", cValue, cBtn, 2);
  drawText(static_cast<int>(sUi.toggleDist.x) + 8, static_cast<int>(sUi.toggleDist.y) + 20,
           state.useMiles ? "MI" : "KM", cValue, cBtn, 2);

  ESP_LOGI(kTag, "wifi/locale config screen drawn w=%u h=%u", w, h);
}

Action hitTest(uint16_t x, uint16_t y) {
  if (!sUi.valid) {
    return Action::None;
  }
  if (contains(sUi.toggleClock, x, y)) {
    return Action::ToggleClock;
  }
  if (contains(sUi.toggleTemp, x, y)) {
    return Action::ToggleTemp;
  }
  if (contains(sUi.toggleDist, x, y)) {
    return Action::ToggleDistance;
  }
  if (sUi.wifiButtons) {
    if (contains(sUi.retry, x, y)) {
      return Action::RetryWifi;
    }
    if (contains(sUi.scan, x, y)) {
      return Action::OpenWifiList;
    }
    if (contains(sUi.offline, x, y)) {
      return Action::OfflineMode;
    }
  }
  return Action::None;
}

void markTouch(uint16_t x, uint16_t y) {
  const uint16_t w = AppConfig::kScreenWidth;
  const uint16_t h = AppConfig::kScreenHeight;
  if (w == 0 || h == 0) {
    return;
  }
  if (x >= w || y >= h) {
    return;
  }

  const uint16_t marker = rgb565(255, 210, 40);
  const uint16_t px = x > 1 ? static_cast<uint16_t>(x - 1) : 0;
  const uint16_t py = y > 1 ? static_cast<uint16_t>(y - 1) : 0;
  (void)display_spi::fillRect(px, py, 3, 3, marker);
}

void showWifiList(const char* const* labels, uint16_t count) {
  const uint16_t w = AppConfig::kScreenWidth;
  const uint16_t h = AppConfig::kScreenHeight;
  if (w == 0 || h == 0) {
    sWifiList.valid = false;
    return;
  }

  const uint16_t cBg = rgb565(8, 10, 18);
  const uint16_t cHeader = rgb565(18, 28, 48);
  const uint16_t cRow = rgb565(20, 30, 50);
  const uint16_t cRowAlt = rgb565(16, 24, 42);
  const uint16_t cText = rgb565(210, 225, 245);
  const uint16_t cSubtle = rgb565(130, 160, 205);
  const uint16_t cBack = rgb565(120, 58, 24);

  const uint16_t headerH = 34;
  const uint16_t listY = 38;
  const uint16_t rowH = 19;
  const uint16_t maxRows = static_cast<uint16_t>((h > listY) ? ((h - listY) / rowH) : 0);
  const uint16_t shown = count < maxRows ? count : maxRows;

  sUi.valid = false;
  sWifiList = {};
  sWifiList.valid = true;
  sWifiList.back = {static_cast<uint16_t>(w - 70), 6, 62, 22};
  sWifiList.startY = listY;
  sWifiList.rowH = rowH;
  sWifiList.count = shown;

  (void)display_spi::clear(cBg);
  (void)display_spi::fillRect(0, 0, w, headerH, cHeader);
  drawText(10, 10, "SELECT WIFI", cText, cHeader, 3);
  drawButton(sWifiList.back, "BACK", cBack, cText, 2);

  if (shown == 0) {
    drawText(14, 58, "NO NETWORKS FOUND", cSubtle, cBg, 2);
    return;
  }

  for (uint16_t i = 0; i < shown; ++i) {
    const uint16_t y = static_cast<uint16_t>(listY + i * rowH);
    const uint16_t rowColor = (i & 1U) == 0U ? cRow : cRowAlt;
    (void)display_spi::fillRect(8, y, static_cast<uint16_t>(w - 16), static_cast<uint16_t>(rowH - 1),
                                rowColor);

    char idxLabel[6] = {};
    std::snprintf(idxLabel, sizeof(idxLabel), "%u:", static_cast<unsigned>(i + 1));
    drawText(12, static_cast<int>(y + 4), idxLabel, cSubtle, rowColor, 2);

    const char* label = (labels != nullptr && labels[i] != nullptr) ? labels[i] : "";
    drawText(36, static_cast<int>(y + 4), label, cText, rowColor, 2);
  }
}

int hitTestWifiListRow(uint16_t x, uint16_t y, uint16_t count) {
  if (!sWifiList.valid) {
    return -2;
  }
  if (contains(sWifiList.back, x, y)) {
    return -1;
  }
  if (sWifiList.rowH == 0 || sWifiList.count == 0) {
    return -2;
  }
  if (count < sWifiList.count) {
    sWifiList.count = count;
  }
  if (x < 8 || x >= static_cast<uint16_t>(AppConfig::kScreenWidth - 8)) {
    return -2;
  }
  if (y < sWifiList.startY) {
    return -2;
  }
  const uint16_t rel = static_cast<uint16_t>(y - sWifiList.startY);
  const uint16_t row = static_cast<uint16_t>(rel / sWifiList.rowH);
  if (row >= sWifiList.count) {
    return -2;
  }
  return static_cast<int>(row);
}

}  // namespace config_screen
