#include "ConfigScreenEspIdf.h"

#include "DisplaySpiEspIdf.h"
#include "AppConfig.h"
#include "Font5x7Classic.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#include <cstdio>
#include <cstdint>
#include <string>

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
bool sLvglReady = false;
lv_display_t* sLvglDisplay = nullptr;
void* sLvglBuf = nullptr;
size_t sLvglBufSize = 0;
constexpr uint32_t kLvTickMs = 10;

void lvFlushCb(lv_display_t* display, const lv_area_t* area, uint8_t* pxMap) {
  const int32_t x1 = area->x1;
  const int32_t y1 = area->y1;
  const int32_t x2 = area->x2;
  const int32_t y2 = area->y2;
  if (x2 < x1 || y2 < y1) {
    lv_display_flush_ready(display);
    return;
  }
  const uint16_t w = static_cast<uint16_t>(x2 - x1 + 1);
  const uint16_t h = static_cast<uint16_t>(y2 - y1 + 1);
  (void)display_spi::drawRgb565(static_cast<uint16_t>(x1), static_cast<uint16_t>(y1), w, h,
                                reinterpret_cast<const uint16_t*>(pxMap));
  lv_display_flush_ready(display);
}

bool ensureLvglReady() {
  if (sLvglReady) {
    return true;
  }

  if (!lv_is_initialized()) {
    lv_init();
  }
  const uint16_t w = AppConfig::kScreenWidth;
  const uint16_t h = AppConfig::kScreenHeight;
  constexpr size_t lines = 20;
  sLvglBufSize = static_cast<size_t>(w) * lines * sizeof(lv_color16_t);
  sLvglBuf = heap_caps_malloc(sLvglBufSize, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (sLvglBuf == nullptr) {
    ESP_LOGW(kTag, "lvgl buffer alloc failed");
    return false;
  }

  sLvglDisplay = lv_display_create(w, h);
  if (sLvglDisplay == nullptr) {
    ESP_LOGW(kTag, "lvgl display create failed");
    return false;
  }
  lv_display_set_buffers(sLvglDisplay, sLvglBuf, nullptr, sLvglBufSize, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_color_format(sLvglDisplay, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(sLvglDisplay, lvFlushCb);

  sLvglReady = true;
  return true;
}

void lvRunFrames(uint32_t ms) {
  if (!sLvglReady) {
    return;
  }
  const uint32_t steps = ms / kLvTickMs;
  for (uint32_t i = 0; i < steps; ++i) {
    lv_tick_inc(kLvTickMs);
    (void)lv_timer_handler();
  }
}

void drawWithLvgl(const config_screen::ViewState& state, const UiLayout& layout) {
  if (!ensureLvglReady()) {
    return;
  }

  lv_obj_t* scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x090C16), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(scr, 0, 0);
  lv_obj_set_style_radius(scr, 0, 0);
  lv_obj_set_size(scr, AppConfig::kScreenWidth, AppConfig::kScreenHeight);

  lv_obj_t* header = lv_obj_create(scr);
  lv_obj_set_style_bg_color(header, lv_color_hex(0x121C30), 0);
  lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(header, 0, 0);
  lv_obj_set_style_radius(header, 0, 0);
  lv_obj_set_size(header, AppConfig::kScreenWidth, 34);
  lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);

  lv_obj_t* title = lv_label_create(scr);
  lv_label_set_text(title, "WIFI LOCALE CONFIG");
  lv_obj_set_style_text_color(title, lv_color_hex(0xD2E1F5), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 10);

  auto addBtn = [&](const Rect& r, const char* text, uint32_t bg, uint32_t fg) {
    lv_obj_t* btn = lv_btn_create(scr);
    lv_obj_set_size(btn, r.w, r.h);
    lv_obj_set_pos(btn, r.x, r.y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 5, 0);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(fg), 0);
    lv_obj_center(lbl);
  };

  lv_obj_t* wifiLabel = lv_label_create(scr);
  lv_label_set_text(wifiLabel, "WIFI");
  lv_obj_set_style_text_color(wifiLabel, lv_color_hex(0xD2E1F5), 0);
  lv_obj_set_pos(wifiLabel, 16, 58);

  lv_obj_t* wifiState = lv_label_create(scr);
  lv_label_set_text(wifiState,
                    state.wifiConnected ? "CONNECTED"
                                        : (state.hasStoredCreds ? "RETRY NEEDED" : "NEEDS SETUP"));
  lv_obj_set_style_text_color(
      wifiState, lv_color_hex(state.wifiConnected ? 0x1E8C3C : (state.hasStoredCreds ? 0xAA6E0A : 0x82A0CD)),
      0);
  lv_obj_set_pos(wifiState, 16, 80);

  if (state.showWifiButtons) {
    addBtn(layout.retry, "RETRY", 0x224270, 0xD2E1F5);
    addBtn(layout.scan, "SCAN", 0x224270, 0xD2E1F5);
    addBtn(layout.offline, "OFFLINE", 0x783A18, 0xD2E1F5);
  }

  lv_obj_t* localeLabel = lv_label_create(scr);
  lv_label_set_text(localeLabel, "LOCALE");
  lv_obj_set_style_text_color(localeLabel, lv_color_hex(0xD2E1F5), 0);
  lv_obj_set_pos(localeLabel, 16, 182);

  lv_obj_t* localeState = lv_label_create(scr);
  lv_label_set_text(localeState, state.wifiConnected ? "AUTO READY" : "MANUAL PENDING");
  lv_obj_set_style_text_color(localeState, lv_color_hex(state.wifiConnected ? 0x1E8C3C : 0xAA6E0A), 0);
  lv_obj_set_pos(localeState, 16, 202);

  addBtn(layout.toggleClock, state.use24HourClock ? "TIME 24H" : "TIME 12H", 0x224270, 0xFFE46E);
  addBtn(layout.toggleTemp, state.useFahrenheit ? "TEMP F" : "TEMP C", 0x224270, 0xFFE46E);
  addBtn(layout.toggleDist, state.useMiles ? "DIST MI" : "DIST KM", 0x224270, 0xFFE46E);

  lv_screen_load(scr);
  lvRunFrames(40);
}

void drawWifiListWithLvgl(const char* const* labels, uint16_t shown, const WifiListLayout& listLayout) {
  if (!ensureLvglReady()) {
    return;
  }

  lv_obj_t* scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x080A12), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(scr, 0, 0);
  lv_obj_set_style_radius(scr, 0, 0);
  lv_obj_set_size(scr, AppConfig::kScreenWidth, AppConfig::kScreenHeight);

  lv_obj_t* header = lv_obj_create(scr);
  lv_obj_set_style_bg_color(header, lv_color_hex(0x121C30), 0);
  lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(header, 0, 0);
  lv_obj_set_style_radius(header, 0, 0);
  lv_obj_set_size(header, AppConfig::kScreenWidth, 34);
  lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);

  lv_obj_t* title = lv_label_create(scr);
  lv_label_set_text(title, "SELECT WIFI");
  lv_obj_set_style_text_color(title, lv_color_hex(0xD2E1F5), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 10);

  lv_obj_t* backBtn = lv_btn_create(scr);
  lv_obj_set_pos(backBtn, listLayout.back.x, listLayout.back.y);
  lv_obj_set_size(backBtn, listLayout.back.w, listLayout.back.h);
  lv_obj_set_style_bg_color(backBtn, lv_color_hex(0x783A18), 0);
  lv_obj_set_style_border_width(backBtn, 0, 0);
  lv_obj_set_style_radius(backBtn, 5, 0);
  lv_obj_t* backLbl = lv_label_create(backBtn);
  lv_label_set_text(backLbl, "BACK");
  lv_obj_set_style_text_color(backLbl, lv_color_hex(0xD2E1F5), 0);
  lv_obj_center(backLbl);

  if (shown == 0) {
    lv_obj_t* none = lv_label_create(scr);
    lv_label_set_text(none, "NO NETWORKS FOUND");
    lv_obj_set_style_text_color(none, lv_color_hex(0x82A0CD), 0);
    lv_obj_set_pos(none, 14, 58);
  } else {
    for (uint16_t i = 0; i < shown; ++i) {
      const uint16_t y = static_cast<uint16_t>(listLayout.startY + i * listLayout.rowH);
      lv_obj_t* row = lv_obj_create(scr);
      lv_obj_set_pos(row, 8, y);
      lv_obj_set_size(row, static_cast<uint16_t>(AppConfig::kScreenWidth - 16),
                      static_cast<uint16_t>(listLayout.rowH - 1));
      lv_obj_set_style_bg_color(row, lv_color_hex((i & 1U) == 0U ? 0x141E32 : 0x10182A), 0);
      lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(row, 0, 0);
      lv_obj_set_style_radius(row, 2, 0);
      lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_style_pad_all(row, 0, 0);

      char idxLabel[12] = {};
      std::snprintf(idxLabel, sizeof(idxLabel), "%u:", static_cast<unsigned>(i + 1));
      lv_obj_t* idx = lv_label_create(scr);
      lv_label_set_text(idx, idxLabel);
      lv_obj_set_style_text_color(idx, lv_color_hex(0x82A0CD), 0);
      lv_obj_set_style_text_font(idx, LV_FONT_DEFAULT, 0);
      lv_obj_set_pos(idx, 12, static_cast<int32_t>(y + 2));

      const char* label = (labels != nullptr && labels[i] != nullptr) ? labels[i] : "";
      lv_obj_t* txt = lv_label_create(scr);
      lv_label_set_text(txt, label);
      lv_obj_set_style_text_color(txt, lv_color_hex(0xD2E1F5), 0);
      lv_obj_set_style_text_font(txt, LV_FONT_DEFAULT, 0);
      lv_obj_set_pos(txt, 36, static_cast<int32_t>(y + 2));
      lv_label_set_long_mode(txt, LV_LABEL_LONG_CLIP);
      lv_obj_set_width(txt, AppConfig::kScreenWidth - 48);
    }
  }

  lv_screen_load(scr);
  lvRunFrames(50);
}

void drawStatusWithLvgl(const char* title, const char* subtitle, bool isError) {
  if (!ensureLvglReady()) {
    return;
  }
  lv_obj_t* scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x080A12), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(scr, 0, 0);
  lv_obj_set_style_radius(scr, 0, 0);
  lv_obj_set_size(scr, AppConfig::kScreenWidth, AppConfig::kScreenHeight);

  lv_obj_t* header = lv_obj_create(scr);
  lv_obj_set_style_bg_color(header, lv_color_hex(0x121C30), 0);
  lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(header, 0, 0);
  lv_obj_set_style_radius(header, 0, 0);
  lv_obj_set_size(header, AppConfig::kScreenWidth, 34);
  lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);

  lv_obj_t* titleLbl = lv_label_create(scr);
  lv_label_set_text(titleLbl, title != nullptr ? title : "WIFI");
  lv_obj_set_style_text_color(titleLbl, lv_color_hex(0xD2E1F5), 0);
  lv_obj_align(titleLbl, LV_ALIGN_TOP_LEFT, 10, 10);

  lv_obj_t* card = lv_obj_create(scr);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x161E32), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_radius(card, 8, 0);
  lv_obj_set_size(card, static_cast<uint16_t>(AppConfig::kScreenWidth - 24), 90);
  lv_obj_align(card, LV_ALIGN_CENTER, 0, 10);

  lv_obj_t* msg = lv_label_create(card);
  lv_label_set_text(msg, subtitle != nullptr ? subtitle : "");
  lv_obj_set_style_text_color(msg, lv_color_hex(isError ? 0xE68080 : 0xD2E1F5), 0);
  lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(msg, AppConfig::kScreenWidth - 40);
  lv_obj_center(msg);

  lv_screen_load(scr);
  lvRunFrames(60);
}

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

void drawChar5x7(int x, int y, char c, uint16_t fg, uint16_t bg, int scale) {
  if (c < 0x20 || c > 0x7E) {
    c = '?';
  }
  const size_t idx = static_cast<size_t>(static_cast<uint8_t>(c)) * 5U;
  for (int col = 0; col < 5; ++col) {
    const uint8_t line = font[idx + static_cast<size_t>(col)];
    for (int row = 0; row < 8; ++row) {
      const bool on = ((line >> row) & 0x01U) != 0U;
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
  const std::string s(text);
  for (char c : s) {
    if (c == ' ') {
      penX += scale * 6;
      continue;
    }
    drawChar5x7(penX, y, c, fg, bg, scale);
    penX += scale * 6;
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

  if (ensureLvglReady()) {
    drawWithLvgl(state, sUi);
  } else {
    ESP_LOGW(kTag, "LVGL unavailable for config; using fallback renderer");
    (void)display_spi::clear(cBg);
    (void)display_spi::fillRect(0, 0, w, 34, cHeader);
    (void)display_spi::fillRect(8, cardY1, w - 16, cardH, cCard);
    (void)display_spi::fillRect(8, cardY2, w - 16, cardH, cCard);

    drawText(10, 10, "WIFI LOCALE CONFIG", cText, cHeader, 2);

    const uint16_t wifiStateColor =
        state.wifiConnected ? cGood : (state.hasStoredCreds ? cWarn : cSubtle);
    const uint16_t localeStateColor = state.wifiConnected ? cGood : cWarn;

    drawText(16, 58, "WIFI", cText, cCard, 2);
    drawText(16, 80,
             state.wifiConnected ? "CONNECTED"
                                 : (state.hasStoredCreds ? "RETRY NEEDED" : "NEEDS SETUP"),
             wifiStateColor, cCard, 1);
    if (state.showWifiButtons) {
      drawButton(sUi.retry, "RETRY", cBtn, cText, 1);
      drawButton(sUi.scan, "SCAN", cBtn, cText, 1);
      drawButton(sUi.offline, "OFFLINE", cBtnWarn, cText, 1);
    }

    drawText(16, static_cast<int>(62 + h / 2), "LOCALE", cText, cCard, 2);
    drawText(16, static_cast<int>(84 + h / 2), state.wifiConnected ? "AUTO READY" : "MANUAL PENDING",
             localeStateColor, cCard, 1);
    drawButton(sUi.toggleClock, "TIME", cBtn, cText, 1);
    drawButton(sUi.toggleTemp, "TEMP", cBtn, cText, 1);
    drawButton(sUi.toggleDist, "DIST", cBtn, cText, 1);

    drawText(static_cast<int>(sUi.toggleClock.x) + 8, static_cast<int>(sUi.toggleClock.y) + 20,
             state.use24HourClock ? "24H" : "12H", cValue, cBtn, 1);
    drawText(static_cast<int>(sUi.toggleTemp.x) + 8, static_cast<int>(sUi.toggleTemp.y) + 20,
             state.useFahrenheit ? "F" : "C", cValue, cBtn, 1);
    drawText(static_cast<int>(sUi.toggleDist.x) + 8, static_cast<int>(sUi.toggleDist.y) + 20,
             state.useMiles ? "MI" : "KM", cValue, cBtn, 1);
  }

  ESP_LOGI(kTag, "wifi/locale config screen drawn w=%u h=%u", w, h);
}

void showWifiScanInterstitial() {
  const uint16_t w = AppConfig::kScreenWidth;
  const uint16_t h = AppConfig::kScreenHeight;
  if (w == 0 || h == 0) {
    return;
  }

  const uint16_t cBg = rgb565(8, 10, 18);
  const uint16_t cHeader = rgb565(18, 28, 48);
  const uint16_t cText = rgb565(210, 225, 245);
  const uint16_t cSubtle = rgb565(130, 160, 205);

  sUi.valid = false;
  sWifiList.valid = false;

  if (ensureLvglReady()) {
    drawStatusWithLvgl("SCANNING WIFI", "SEARCHING FOR NETWORKS...\nFIRST SCAN CAN TAKE 10-12S",
                       false);
    return;
  }

  (void)display_spi::clear(cBg);
  (void)display_spi::fillRect(0, 0, w, 34, cHeader);
  drawText(10, 10, "SCANNING WIFI", cText, cHeader, 2);
  drawText(18, static_cast<int>(h / 2) - 10, "SEARCHING FOR NETWORKS...", cText, cBg, 1);
  drawText(18, static_cast<int>(h / 2) + 8, "FIRST SCAN CAN TAKE 10-12S", cSubtle, cBg, 1);
}

void showWifiStatus(const char* title, const char* subtitle, bool isError) {
  const uint16_t w = AppConfig::kScreenWidth;
  const uint16_t h = AppConfig::kScreenHeight;
  if (w == 0 || h == 0) {
    return;
  }
  sUi.valid = false;
  sWifiList.valid = false;

  if (ensureLvglReady()) {
    drawStatusWithLvgl(title, subtitle, isError);
    return;
  }

  const uint16_t cBg = rgb565(8, 10, 18);
  const uint16_t cHeader = rgb565(18, 28, 48);
  const uint16_t cText = rgb565(210, 225, 245);
  const uint16_t cErr = rgb565(230, 120, 120);
  (void)display_spi::clear(cBg);
  (void)display_spi::fillRect(0, 0, w, 34, cHeader);
  drawText(10, 10, title != nullptr ? title : "WIFI", cText, cHeader, 2);
  drawText(14, static_cast<int>(h / 2), subtitle != nullptr ? subtitle : "", isError ? cErr : cText, cBg,
           1);
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

  if (ensureLvglReady()) {
    drawWifiListWithLvgl(labels, shown, sWifiList);
    return;
  }
  ESP_LOGW(kTag, "LVGL unavailable for wifi list; using fallback renderer");

  const uint16_t cBg = rgb565(8, 10, 18);
  const uint16_t cHeader = rgb565(18, 28, 48);
  const uint16_t cRow = rgb565(20, 30, 50);
  const uint16_t cRowAlt = rgb565(16, 24, 42);
  const uint16_t cText = rgb565(210, 225, 245);
  const uint16_t cSubtle = rgb565(130, 160, 205);
  const uint16_t cBack = rgb565(120, 58, 24);

  (void)display_spi::clear(cBg);
  (void)display_spi::fillRect(0, 0, w, headerH, cHeader);
  drawText(10, 10, "SELECT WIFI", cText, cHeader, 2);
  drawButton(sWifiList.back, "BACK", cBack, cText, 1);

  if (shown == 0) {
    drawText(14, 58, "NO NETWORKS FOUND", cSubtle, cBg, 1);
    return;
  }

  for (uint16_t i = 0; i < shown; ++i) {
    const uint16_t y = static_cast<uint16_t>(listY + i * rowH);
    const uint16_t rowColor = (i & 1U) == 0U ? cRow : cRowAlt;
    (void)display_spi::fillRect(8, y, static_cast<uint16_t>(w - 16), static_cast<uint16_t>(rowH - 1),
                                rowColor);

    char idxLabel[6] = {};
    std::snprintf(idxLabel, sizeof(idxLabel), "%u:", static_cast<unsigned>(i + 1));
    drawText(12, static_cast<int>(y + 4), idxLabel, cSubtle, rowColor, 1);

    const char* label = (labels != nullptr && labels[i] != nullptr) ? labels[i] : "";
    drawText(36, static_cast<int>(y + 4), label, cText, rowColor, 1);
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
