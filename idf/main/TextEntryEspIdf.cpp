#include "TextEntryEspIdf.h"

#include "AppConfig.h"
#include "DisplaySpiEspIdf.h"
#include "Font5x7Classic.h"
#include "TouchInputEspIdf.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace {

struct Rect {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t w = 0;
  uint16_t h = 0;
};

enum class KeyAction : uint8_t {
  Char = 0,
  Backspace,
  Space,
  ModeUpper,
  ModeLower,
  ModeNumSym,
  Done,
  Cancel,
};

struct Key {
  Rect r;
  KeyAction action = KeyAction::Char;
  char ch = '\0';
  std::string label;
};

enum class KeyboardMode : uint8_t {
  Upper = 0,
  Lower,
  NumSym,
};

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

bool contains(const Rect& r, uint16_t x, uint16_t y) {
  if (r.w == 0 || r.h == 0) {
    return false;
  }
  return x >= r.x && x < static_cast<uint16_t>(r.x + r.w) && y >= r.y &&
         y < static_cast<uint16_t>(r.y + r.h);
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

void drawText5x7(int x, int y, const std::string& text, uint16_t fg, uint16_t bg, int scale) {
  int penX = x;
  for (char c : text) {
    if (c == ' ') {
      penX += scale * 6;
      continue;
    }
    drawChar5x7(penX, y, c, fg, bg, scale);
    penX += scale * 6;
  }
}

void drawButton(const Key& key, uint16_t bg, uint16_t fg, int scale) {
  (void)display_spi::fillRect(key.r.x, key.r.y, key.r.w, key.r.h, bg);
  const int tx = static_cast<int>(key.r.x) + 4;
  const int ty = static_cast<int>(key.r.y) + 8;
  drawText5x7(tx, ty, key.label, fg, bg, scale);
}

std::string maskOrRaw(const std::string& input, bool mask) {
  std::string out = input;
  if (mask) {
    out.assign(input.size(), '*');
  }
  out.push_back('_');
  if (out.size() > 32) {
    out = out.substr(out.size() - 32);
  }
  return out;
}

bool waitForTap(uint16_t& outX, uint16_t& outY) {
  touch_input::Point p;
  while (!touch_input::read(p)) {
    vTaskDelay(pdMS_TO_TICKS(15));
  }
  outX = p.x;
  outY = p.y;
  vTaskDelay(pdMS_TO_TICKS(AppConfig::kTouchDebounceMs));
  while (touch_input::read(p)) {
    vTaskDelay(pdMS_TO_TICKS(15));
  }
  return true;
}

void addCharRow(std::vector<Key>& keys, const char* chars, uint16_t startX, uint16_t y,
                uint16_t keyW, uint16_t keyH, uint16_t gap) {
  if (chars == nullptr) {
    return;
  }
  uint16_t x = startX;
  for (const char* p = chars; *p != '\0'; ++p) {
    Key k = {};
    k.r = Rect{x, y, keyW, keyH};
    k.action = KeyAction::Char;
    k.ch = *p;
    k.label.assign(1, *p);
    keys.push_back(k);
    x = static_cast<uint16_t>(x + keyW + gap);
  }
}

std::vector<Key> buildKeys(KeyboardMode mode) {
  std::vector<Key> keys;
  keys.reserve(64);

  constexpr uint16_t keyW = 29;
  constexpr uint16_t keyH = 28;
  constexpr uint16_t gap = 2;
  constexpr uint16_t y0 = 72;
  constexpr uint16_t y1 = 102;
  constexpr uint16_t y2 = 132;
  constexpr uint16_t y3 = 166;

  if (mode == KeyboardMode::Upper) {
    addCharRow(keys, "QWERTYUIOP", 6, y0, keyW, keyH, gap);
    addCharRow(keys, "ASDFGHJKL", 22, y1, keyW, keyH, gap);
    addCharRow(keys, "ZXCVBNM", 52, y2, keyW, keyH, gap);
  } else if (mode == KeyboardMode::Lower) {
    addCharRow(keys, "qwertyuiop", 6, y0, keyW, keyH, gap);
    addCharRow(keys, "asdfghjkl", 22, y1, keyW, keyH, gap);
    addCharRow(keys, "zxcvbnm", 52, y2, keyW, keyH, gap);
  } else {
    addCharRow(keys, "1234567890", 6, y0, keyW, keyH, gap);
    addCharRow(keys, "!@#$%^&*()", 6, y1, keyW, keyH, gap);
    addCharRow(keys, "-_=+[]{}\\|", 6, y2, keyW, keyH, gap);
    addCharRow(keys, ";:'\",.<>/?", 6, static_cast<uint16_t>(y2 + keyH + gap), keyW, keyH, gap);
  }

  Key bk = {};
  bk.r = Rect{269, y2, 45, keyH};
  bk.action = KeyAction::Backspace;
  bk.label = "<-";
  keys.push_back(bk);

  Key upper = {};
  upper.r = Rect{4, y3, 46, keyH};
  upper.action = KeyAction::ModeUpper;
  upper.label = "ABC";
  keys.push_back(upper);

  Key lower = {};
  lower.r = Rect{52, y3, 46, keyH};
  lower.action = KeyAction::ModeLower;
  lower.label = "abc";
  keys.push_back(lower);

  Key nums = {};
  nums.r = Rect{100, y3, 62, keyH};
  nums.action = KeyAction::ModeNumSym;
  nums.label = "123#+";
  keys.push_back(nums);

  Key space = {};
  space.r = Rect{164, y3, 72, keyH};
  space.action = KeyAction::Space;
  space.label = "SPACE";
  keys.push_back(space);

  Key ok = {};
  ok.r = Rect{238, y3, 36, keyH};
  ok.action = KeyAction::Done;
  ok.label = "OK";
  keys.push_back(ok);

  Key cancel = {};
  cancel.r = Rect{276, y3, 40, keyH};
  cancel.action = KeyAction::Cancel;
  cancel.label = "ESC";
  keys.push_back(cancel);

  return keys;
}

}  // namespace

namespace text_entry {

bool prompt(const Options& options, std::string& outValue) {
  std::string input = options.initial;
  KeyboardMode mode = KeyboardMode::Upper;

  const uint16_t cBg = rgb565(8, 12, 20);
  const uint16_t cHeader = rgb565(18, 28, 48);
  const uint16_t cField = rgb565(20, 30, 52);
  const uint16_t cKey = rgb565(34, 52, 84);
  const uint16_t cMode = rgb565(68, 78, 108);
  const uint16_t cOk = rgb565(28, 124, 58);
  const uint16_t cDanger = rgb565(130, 54, 40);
  const uint16_t cText = rgb565(228, 238, 252);
  const uint16_t cSubtle = rgb565(152, 178, 212);
  const uint16_t cActive = rgb565(60, 118, 210);

  for (;;) {
    const std::vector<Key> keys = buildKeys(mode);

    (void)display_spi::clear(cBg);
    (void)display_spi::fillRect(0, 0, AppConfig::kScreenWidth, 24, cHeader);
    (void)display_spi::fillRect(4, 28, static_cast<uint16_t>(AppConfig::kScreenWidth - 8), 40, cField);

    drawText5x7(8, 6, options.title.empty() ? std::string("WIFI PASSWORD") : options.title, cText, cHeader, 1);
    if (!options.subtitle.empty()) {
      drawText5x7(8, 34, options.subtitle, cSubtle, cField, 1);
    }
    const std::string content = maskOrRaw(input, options.maskInput);
    drawText5x7(8, 48, content, cText, cField, 1);

    for (const Key& key : keys) {
      uint16_t bg = cKey;
      if (key.action == KeyAction::ModeUpper || key.action == KeyAction::ModeLower ||
          key.action == KeyAction::ModeNumSym) {
        bg = cMode;
      } else if (key.action == KeyAction::Done) {
        bg = cOk;
      } else if (key.action == KeyAction::Cancel) {
        bg = cDanger;
      }
      if ((mode == KeyboardMode::Upper && key.action == KeyAction::ModeUpper) ||
          (mode == KeyboardMode::Lower && key.action == KeyAction::ModeLower) ||
          (mode == KeyboardMode::NumSym && key.action == KeyAction::ModeNumSym)) {
        bg = cActive;
      }
      drawButton(key, bg, cText, 1);
    }

    uint16_t x = 0;
    uint16_t y = 0;
    (void)waitForTap(x, y);

    bool handled = false;
    for (const Key& key : keys) {
      if (!contains(key.r, x, y)) {
        continue;
      }
      handled = true;
      switch (key.action) {
        case KeyAction::Char:
          if (input.size() < options.maxLen) {
            input.push_back(key.ch);
          }
          break;
        case KeyAction::Backspace:
          if (!input.empty()) {
            input.pop_back();
          }
          break;
        case KeyAction::Space:
          if (input.size() < options.maxLen) {
            input.push_back(' ');
          }
          break;
        case KeyAction::ModeUpper:
          mode = KeyboardMode::Upper;
          break;
        case KeyAction::ModeLower:
          mode = KeyboardMode::Lower;
          break;
        case KeyAction::ModeNumSym:
          mode = KeyboardMode::NumSym;
          break;
        case KeyAction::Done:
          outValue = input;
          return true;
        case KeyAction::Cancel:
          return false;
      }
      break;
    }

    if (!handled) {
      vTaskDelay(pdMS_TO_TICKS(30));
    }
  }
}

}  // namespace text_entry
