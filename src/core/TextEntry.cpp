#include "core/TextEntry.h"

#include <vector>

#include "AppConfig.h"
#include "core/TouchMapper.h"

namespace {
constexpr int16_t kHeaderH = 28;

constexpr int16_t kAlphaKW = 28;
constexpr int16_t kAlphaKH = 28;
constexpr int16_t kAlphaKG = 3;
constexpr int16_t kAlphaKS = kAlphaKW + kAlphaKG;

constexpr int16_t kAlphaY0 = 52;
constexpr int16_t kAlphaY1 = kAlphaY0 + kAlphaKH + kAlphaKG;
constexpr int16_t kAlphaY2 = kAlphaY1 + kAlphaKH + kAlphaKG;
constexpr int16_t kAlphaY3 = kAlphaY2 + kAlphaKH + kAlphaKG;

constexpr int16_t kAlphaRow10 = 6;
constexpr int16_t kAlphaRow9 = 22;

constexpr int16_t kShiftX = 8;
constexpr int16_t kShiftW = 42;
constexpr int16_t kAlphaRow7X = kShiftX + kShiftW + kAlphaKG;
constexpr int16_t kBackspaceX = 270;
constexpr int16_t kBackspaceW = 42;

constexpr int16_t kModeX = 8;
constexpr int16_t kModeW = 50;
constexpr int16_t kSpaceX = kModeX + kModeW + kAlphaKG;
constexpr int16_t kEnterX = 252;
constexpr int16_t kEnterW = 60;
constexpr int16_t kSpaceW = kEnterX - kAlphaKG - kSpaceX;

constexpr int16_t kNumKW = 62;
constexpr int16_t kNumKH = 28;
constexpr int16_t kNumKG = 6;
constexpr int16_t kNumX0 = 10;
constexpr int16_t kNumY0 = 60;

constexpr int16_t kNumRowGap = kNumKH + kNumKG;
constexpr int16_t kNumY1 = kNumY0 + kNumRowGap;
constexpr int16_t kNumY2 = kNumY1 + kNumRowGap;
constexpr int16_t kNumY3 = kNumY2 + kNumRowGap;
constexpr int16_t kNumY4 = kNumY3 + kNumRowGap;

constexpr int16_t kNumBackW = 140;
constexpr int16_t kNumOkW = 140;
constexpr int16_t kNumBackX = 10;
constexpr int16_t kNumOkX = 170;

struct KeyRect {
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;
  String label;
  char ch = '\0';
  uint8_t action = 0;
};

enum KeyAction : uint8_t {
  kChar = 0,
  kBackspace,
  kSpace,
  kToggleMode,
  kDone,
  kToggleSign,
  kDecimal,
};

String makeDisplay(const String& input, bool mask) {
  String out;
  out.reserve(input.length() + 1);
  if (mask) {
    for (size_t i = 0; i < input.length(); ++i) {
      out += '*';
    }
  } else {
    out = input;
  }
  out += "_";
  if (out.length() > 38) {
    out = out.substring(out.length() - 38);
  }
  return out;
}
}  // namespace

TextEntry::TextEntry(TFT_eSPI& tft, XPT2046_Touchscreen& touch) : tft_(tft), touch_(touch) {}

String TextEntry::prompt(const TextEntryOptions& options) {
  bool symMode = false;
  bool capsOn = true;
  String input = options.initial;

  while (true) {
    const uint16_t keyBg = tft_.color565(35, 35, 50);
    const uint16_t keySpecial = tft_.color565(40, 70, 140);
    const uint16_t keyGreen = tft_.color565(20, 140, 60);
    const uint16_t keyActive = tft_.color565(60, 120, 210);

    tft_.fillScreen(TFT_BLACK);
    tft_.setTextDatum(ML_DATUM);
    tft_.setTextColor(tft_.color565(120, 200, 255), TFT_BLACK);
    if (!options.title.isEmpty()) {
      tft_.drawString(options.title, 8, 10, 1);
    }
    if (!options.subtitle.isEmpty()) {
      tft_.setTextColor(tft_.color565(140, 140, 160), TFT_BLACK);
      tft_.drawString(options.subtitle, 8, 22, 1);
    }

    tft_.fillRoundRect(272, 2, 42, 18, 3, keySpecial);
    tft_.setTextDatum(MC_DATUM);
    tft_.setTextColor(TFT_WHITE, keySpecial);
    tft_.drawString("< Back", 293, 11, 1);

    const uint16_t fieldBg = tft_.color565(18, 18, 28);
    tft_.fillRoundRect(4, 30, 312, 26, 4, fieldBg);
    tft_.drawRoundRect(4, 30, 312, 26, 4, tft_.color565(70, 70, 100));

    tft_.setTextDatum(ML_DATUM);
    tft_.setTextColor(TFT_WHITE, fieldBg);
    tft_.drawString(makeDisplay(input, options.maskInput), 10, 43, 2);

    tft_.setTextDatum(MR_DATUM);
    tft_.setTextColor(tft_.color565(130, 130, 130), fieldBg);
    tft_.drawString(String(input.length()), 308, 43, 1);

    std::vector<KeyRect> keys;

    if (options.numericOnly) {
      auto addKey = [&](int x, int y, const char* label, uint8_t action, char ch = '\0') {
        KeyRect k;
        k.x = x;
        k.y = y;
        k.w = kNumKW;
        k.h = kNumKH;
        k.label = label;
        k.action = action;
        k.ch = ch;
        keys.push_back(k);
      };

      addKey(kNumX0 + 0 * (kNumKW + kNumKG), kNumY0, "1", kChar, '1');
      addKey(kNumX0 + 1 * (kNumKW + kNumKG), kNumY0, "2", kChar, '2');
      addKey(kNumX0 + 2 * (kNumKW + kNumKG), kNumY0, "3", kChar, '3');
      addKey(kNumX0 + 0 * (kNumKW + kNumKG), kNumY1, "4", kChar, '4');
      addKey(kNumX0 + 1 * (kNumKW + kNumKG), kNumY1, "5", kChar, '5');
      addKey(kNumX0 + 2 * (kNumKW + kNumKG), kNumY1, "6", kChar, '6');
      addKey(kNumX0 + 0 * (kNumKW + kNumKG), kNumY2, "7", kChar, '7');
      addKey(kNumX0 + 1 * (kNumKW + kNumKG), kNumY2, "8", kChar, '8');
      addKey(kNumX0 + 2 * (kNumKW + kNumKG), kNumY2, "9", kChar, '9');
      addKey(kNumX0 + 0 * (kNumKW + kNumKG), kNumY3, "+/-", kToggleSign);
      addKey(kNumX0 + 1 * (kNumKW + kNumKG), kNumY3, "0", kChar, '0');
      addKey(kNumX0 + 2 * (kNumKW + kNumKG), kNumY3, ".", kDecimal);

      KeyRect back;
      back.x = kNumBackX;
      back.y = kNumY4;
      back.w = kNumBackW;
      back.h = kNumKH;
      back.label = "< X";
      back.action = kBackspace;
      keys.push_back(back);

      KeyRect ok;
      ok.x = kNumOkX;
      ok.y = kNumY4;
      ok.w = kNumOkW;
      ok.h = kNumKH;
      ok.label = "OK";
      ok.action = kDone;
      keys.push_back(ok);
    } else {
      auto addCharRow = [&](const char* chars, int y, int startX, int count) {
        for (int i = 0; i < count; ++i) {
          KeyRect k;
          k.x = startX + i * kAlphaKS;
          k.y = y;
          k.w = kAlphaKW;
          k.h = kAlphaKH;
          k.ch = chars[i];
          if (!symMode && !capsOn) {
            k.label = String((char)tolower(chars[i]));
          } else {
            k.label = String(chars[i]);
          }
          k.action = kChar;
          keys.push_back(k);
        }
      };

      if (symMode) {
        addCharRow("1234567890", kAlphaY0, kAlphaRow10, 10);
        addCharRow("!@#$%^&*()", kAlphaY1, kAlphaRow10, 10);
        addCharRow("-_=+.,/?;'", kAlphaY2, kAlphaRow7X, 7);
      } else {
        addCharRow("QWERTYUIOP", kAlphaY0, kAlphaRow10, 10);
        addCharRow("ASDFGHJKL", kAlphaY1, kAlphaRow9, 9);
        addCharRow("ZXCVBNM", kAlphaY2, kAlphaRow7X, 7);
      }

      auto addAction = [&](int x, int y, int w, const char* label, KeyAction action) {
        KeyRect k;
        k.x = x;
        k.y = y;
        k.w = w;
        k.h = kAlphaKH;
        k.label = label;
        k.action = action;
        keys.push_back(k);
      };

      addAction(kShiftX, kAlphaY2, kShiftW, symMode ? "---" : (capsOn ? "CAP" : "shf"),
                kToggleMode);
      addAction(kBackspaceX, kAlphaY2, kBackspaceW, "< X", kBackspace);
      addAction(kModeX, kAlphaY3, kModeW, symMode ? "ABC" : "123", kToggleMode);
      addAction(kSpaceX, kAlphaY3, kSpaceW, "SPACE", kSpace);
      addAction(kEnterX, kAlphaY3, kEnterW, "OK", kDone);
    }

    for (const auto& k : keys) {
      uint16_t bg = keyBg;
      if (k.action == kDone) {
        bg = keyGreen;
      } else if (k.action != kChar) {
        bg = keySpecial;
      }
      if (k.label == "CAP") {
        bg = keyActive;
      }

      tft_.fillRoundRect(k.x, k.y, k.w, k.h, 3, bg);
      tft_.setTextDatum(MC_DATUM);
      tft_.setTextColor(TFT_WHITE, bg);
      tft_.drawString(k.label, k.x + k.w / 2, k.y + k.h / 2, 2);
    }

    uint16_t tx = 0;
    uint16_t ty = 0;
    while (!readTouch(tx, ty)) {
      delay(20);
    }

    if (ty < kHeaderH && tx >= 272) {
      waitForTouchRelease();
      return "__CANCEL__";
    }

    for (const auto& k : keys) {
      if (tx < k.x || tx >= k.x + k.w || ty < k.y || ty >= k.y + k.h) {
        continue;
      }

      waitForTouchRelease();
      if (k.action == kChar) {
        if (input.length() < options.maxLen) {
          char c = k.ch;
          if (!options.numericOnly && !symMode && !capsOn) {
            c = (char)tolower(c);
          }
          input += c;
          if (!options.numericOnly && !symMode && capsOn && input.length() == 1) {
            capsOn = false;
          }
        }
      } else if (k.action == kBackspace) {
        if (input.length() > 0) {
          input.remove(input.length() - 1);
        }
      } else if (k.action == kSpace) {
        if (input.length() < options.maxLen) {
          input += ' ';
        }
      } else if (k.action == kDone) {
        return input;
      } else if (k.action == kToggleMode) {
        if (k.label == "CAP" || k.label == "shf") {
          capsOn = !capsOn;
        } else {
          symMode = !symMode;
          if (!symMode) {
            capsOn = true;
          }
        }
      } else if (k.action == kToggleSign) {
        if (input.startsWith("-")) {
          input.remove(0, 1);
        } else {
          if (input.length() < options.maxLen) {
            input = "-" + input;
          }
        }
      } else if (k.action == kDecimal) {
        if (input.indexOf('.') < 0) {
          if (input.isEmpty()) {
            input = "0.";
          } else if (input.length() < options.maxLen) {
            input += '.';
          }
        }
      }
      break;
    }
  }
}

bool TextEntry::readTouch(uint16_t& x, uint16_t& y) {
  if (!AppConfig::kTouchEnabled) {
    return false;
  }
  if (!touch_.touched()) {
    return false;
  }

  TS_Point raw = touch_.getPoint();
  TouchPoint mapped;
  if (!TouchMapper::mapRaw(raw, mapped)) {
    return false;
  }

  x = mapped.x;
  y = mapped.y;
  delay(AppConfig::kTouchDebounceMs);
  return true;
}

void TextEntry::waitForTouchRelease() {
  if (!AppConfig::kTouchEnabled) {
    return;
  }
  while (touch_.touched()) {
    delay(15);
  }
}
