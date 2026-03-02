// TouchCalibration.cpp — display mode and touch screen calibration.

#include "TouchCalibration.h"

#include "AppConfig.h"
#include "ConfigScreenEspIdf.h"
#include "DisplaySpiEspIdf.h"
#include "TouchInputEspIdf.h"
#include "platform/Platform.h"
#include "platform/Prefs.h"

#include "esp_log.h"

#include <algorithm>
#include <cstdint>

namespace {
constexpr const char* kTouchTag = "touch";
}

static void drawCalibrationTarget(uint16_t x, uint16_t y, uint16_t color) {
  const uint16_t dot = 8;
  const uint16_t arm = 16;
  const uint16_t halfDot = dot / 2;
  (void)display_spi::fillRect(static_cast<uint16_t>(x - halfDot), static_cast<uint16_t>(y - 1), dot, 3,
                              color);
  (void)display_spi::fillRect(static_cast<uint16_t>(x - 1), static_cast<uint16_t>(y - halfDot), 3, dot,
                              color);
  (void)display_spi::fillRect(static_cast<uint16_t>(x - 1), static_cast<uint16_t>(y - arm), 3,
                              static_cast<uint16_t>(arm * 2), color);
  (void)display_spi::fillRect(static_cast<uint16_t>(x - arm), static_cast<uint16_t>(y - 1),
                              static_cast<uint16_t>(arm * 2), 3, color);
}

static void showPostCalibrationColorCheck() {
  const uint16_t w = display_spi::width();
  const uint16_t h = display_spi::height();
  if (w < 40 || h < 40) {
    return;
  }
  const uint16_t halfW = w / 2;
  const uint16_t halfH = h / 2;
  const uint16_t cBlack = 0x0000;
  const uint16_t cWhite = 0xFFFF;
  const uint16_t cRed = 0xF800;
  const uint16_t cGreen = 0x07E0;
  const uint16_t cBlue = 0x001F;

  (void)display_spi::fillRect(0, 0, halfW, halfH, cBlack);
  (void)display_spi::fillRect(halfW, 0, static_cast<uint16_t>(w - halfW), halfH, cWhite);

  const uint16_t lowerY = halfH;
  const uint16_t lowerH = static_cast<uint16_t>(h - halfH);
  const uint16_t third = w / 3;
  (void)display_spi::fillRect(0, lowerY, third, lowerH, cRed);
  (void)display_spi::fillRect(third, lowerY, third, lowerH, cGreen);
  (void)display_spi::fillRect(static_cast<uint16_t>(third * 2), lowerY,
                              static_cast<uint16_t>(w - third * 2), lowerH, cBlue);

  ESP_LOGI(kTouchTag, "post-calibration color check shown (TL black, TR white, RGB bottom)");
  platform::sleepMs(1200);
}

static void drawDisplayModePattern(bool bgr, bool invert) {
  const uint16_t w = display_spi::width();
  const uint16_t h = display_spi::height();
  if (w < 40 || h < 40) {
    return;
  }
  const uint16_t cBlack = 0x0000;
  const uint16_t cWhite = 0xFFFF;
  const uint16_t cRed = 0xF800;
  const uint16_t cGreen = 0x07E0;
  const uint16_t cBlue = 0x001F;
  const uint16_t cYellow = 0xFFE0;

  (void)display_spi::clear(0x0000);
  const uint16_t halfW = w / 2;
  const uint16_t topH = h / 3;
  const uint16_t midY = topH;
  const uint16_t midH = h / 3;
  const uint16_t botY = static_cast<uint16_t>(topH + midH);
  const uint16_t botH = static_cast<uint16_t>(h - botY);

  (void)display_spi::fillRect(0, 0, halfW, topH, cBlack);
  (void)display_spi::fillRect(halfW, 0, static_cast<uint16_t>(w - halfW), topH, cWhite);

  const uint16_t quarter = w / 4;
  (void)display_spi::fillRect(0, midY, quarter, midH, cRed);
  (void)display_spi::fillRect(quarter, midY, quarter, midH, cGreen);
  (void)display_spi::fillRect(static_cast<uint16_t>(quarter * 2), midY, quarter, midH, cBlue);
  (void)display_spi::fillRect(static_cast<uint16_t>(quarter * 3), midY,
                              static_cast<uint16_t>(w - quarter * 3), midH, cYellow);

  const uint16_t leftColor = bgr ? 0x001F : 0x07E0;
  const uint16_t rightColor = invert ? 0xF800 : 0x07E0;
  (void)display_spi::fillRect(0, botY, halfW, botH, leftColor);
  (void)display_spi::fillRect(halfW, botY, static_cast<uint16_t>(w - halfW), botH, rightColor);
}

static bool captureCalibrationPoint(uint16_t targetX, uint16_t targetY, uint16_t& rawX,
                                    uint16_t& rawY, bool requireNearTarget) {
  const uint16_t cBg = 0x0000;
  const uint16_t cTarget = 0xFFFF;

  (void)display_spi::clear(cBg);
  drawCalibrationTarget(targetX, targetY, cTarget);

  const uint32_t timeoutMs = 20000;
  const uint32_t start = platform::millisMs();
  while (platform::millisMs() - start < 1500U) {
    touch_input::Point p;
    if (!touch_input::read(p)) {
      break;
    }
    platform::sleepMs(12);
  }

  uint32_t sumX = 0;
  uint32_t sumY = 0;
  uint16_t count = 0;
  bool touching = false;
  uint16_t minRawX = 0xFFFF;
  uint16_t maxRawX = 0;
  uint16_t minRawY = 0xFFFF;
  uint16_t maxRawY = 0;
  constexpr uint16_t kMinSamples = 8;
  constexpr uint16_t kMaxRawJitter = 180;
  constexpr int32_t kNearRadiusPx = 40;

  while (platform::millisMs() - start < timeoutMs) {
    touch_input::Point p;
    if (touch_input::read(p)) {
      touching = true;
      if (requireNearTarget) {
        const int32_t dx = static_cast<int32_t>(p.x) - static_cast<int32_t>(targetX);
        const int32_t dy = static_cast<int32_t>(p.y) - static_cast<int32_t>(targetY);
        if (dx < -kNearRadiusPx || dx > kNearRadiusPx || dy < -kNearRadiusPx ||
            dy > kNearRadiusPx) {
          platform::sleepMs(20);
          continue;
        }
      }
      if (count < 24) {
        sumX += p.rawX;
        sumY += p.rawY;
        minRawX = std::min<uint16_t>(minRawX, p.rawX);
        maxRawX = std::max<uint16_t>(maxRawX, p.rawX);
        minRawY = std::min<uint16_t>(minRawY, p.rawY);
        maxRawY = std::max<uint16_t>(maxRawY, p.rawY);
        ++count;
      }
      if (requireNearTarget) {
        config_screen::markTouch(p.x, p.y);
      }
      platform::sleepMs(20);
      continue;
    }

    if (touching) {
      const uint16_t jitterX = static_cast<uint16_t>(maxRawX - minRawX);
      const uint16_t jitterY = static_cast<uint16_t>(maxRawY - minRawY);
      if (count >= kMinSamples && jitterX <= kMaxRawJitter && jitterY <= kMaxRawJitter) {
        rawX = static_cast<uint16_t>(sumX / count);
        rawY = static_cast<uint16_t>(sumY / count);
        platform::sleepMs(120);
        return true;
      }
      touching = false;
      sumX = 0;
      sumY = 0;
      count = 0;
      minRawX = 0xFFFF;
      maxRawX = 0;
      minRawY = 0xFFFF;
      maxRawY = 0;
    }
    platform::sleepMs(12);
  }
  return false;
}

bool runDisplayModeCalibrationIfNeeded() {
  constexpr const char* kDisplayPrefsNs = "display";
  constexpr const char* kColorSetKey = "color_set";
  constexpr const char* kColorBgrKey = "color_bgr";
  constexpr const char* kInvertSetKey = "inv_set";
  constexpr const char* kInvertOnKey = "inv_on";

  const bool haveColor = platform::prefs::getBool(kDisplayPrefsNs, kColorSetKey, false);
  const bool haveInvert = platform::prefs::getBool(kDisplayPrefsNs, kInvertSetKey, false);

  bool bgr = platform::prefs::getBool(kDisplayPrefsNs, kColorBgrKey, false);
  bool invert = platform::prefs::getBool(kDisplayPrefsNs, kInvertOnKey, true);
  if (!display_spi::applyPanelTuning(bgr, invert, false)) {
    return false;
  }

  if (haveColor && haveInvert) {
    ESP_LOGI(kTouchTag, "display mode already calibrated; using saved bgr=%d invert=%d",
             bgr ? 1 : 0, invert ? 1 : 0);
    return true;
  }

  drawDisplayModePattern(bgr, invert);
  ESP_LOGW(kTouchTag,
           "display mode calibration: tap LEFT half toggles RGB/BGR, RIGHT half toggles invert, "
           "BOTTOM third saves");

  if (!AppConfig::kTouchEnabled) {
    (void)display_spi::applyPanelTuning(bgr, invert, true);
    return true;
  }

  const uint32_t start = platform::millisMs();
  bool held = false;
  while (platform::millisMs() - start < 45000U) {
    touch_input::Point p;
    if (!touch_input::read(p)) {
      held = false;
      platform::sleepMs(15);
      continue;
    }
    if (held) {
      platform::sleepMs(25);
      continue;
    }
    held = true;
    const uint16_t h = display_spi::height();
    const uint16_t w = display_spi::width();
    if (p.y >= (h * 2U) / 3U) {
      (void)display_spi::applyPanelTuning(bgr, invert, true);
      ESP_LOGI(kTouchTag, "display mode saved bgr=%d invert=%d", bgr ? 1 : 0, invert ? 1 : 0);
      (void)display_spi::clear(0x0000);
      return true;
    }
    if (p.x < (w / 2U)) {
      bgr = !bgr;
    } else {
      invert = !invert;
    }
    (void)display_spi::applyPanelTuning(bgr, invert, false);
    drawDisplayModePattern(bgr, invert);
    ESP_LOGI(kTouchTag, "display mode trial bgr=%d invert=%d", bgr ? 1 : 0, invert ? 1 : 0);
  }

  (void)display_spi::applyPanelTuning(bgr, invert, true);
  ESP_LOGW(kTouchTag, "display mode calibration timeout; saved current bgr=%d invert=%d",
           bgr ? 1 : 0, invert ? 1 : 0);
  (void)display_spi::clear(0x0000);
  return true;
}

bool runTouchCalibration(bool force) {
  touch_input::Calibration cal = {};
  if (!force) {
    if (touch_input::loadCalibration(cal)) {
      ESP_LOGI(kTouchTag, "cal loaded minX=%u maxX=%u minY=%u maxY=%u invX=%d invY=%d",
               cal.rawMinX, cal.rawMaxX, cal.rawMinY, cal.rawMaxY,
               cal.invertX ? 1 : 0, cal.invertY ? 1 : 0);
      return true;
    }
  } else if (touch_input::loadCalibration(cal)) {
    ESP_LOGI(kTouchTag, "forcing calibration over stored minX=%u maxX=%u minY=%u maxY=%u",
             cal.rawMinX, cal.rawMaxX, cal.rawMinY, cal.rawMaxY);
  }

  ESP_LOGW(kTouchTag, "no persisted calibration; entering calibration");
  const uint16_t w = AppConfig::kScreenWidth;
  const uint16_t h = AppConfig::kScreenHeight;
  const uint16_t m = 24;
  const uint16_t ulX = m;
  const uint16_t ulY = m;
  const uint16_t urX = static_cast<uint16_t>(w - 1 - m);
  const uint16_t urY = m;
  const uint16_t llX = m;
  const uint16_t llY = static_cast<uint16_t>(h - 1 - m);
  const uint16_t lrX = static_cast<uint16_t>(w - 1 - m);
  const uint16_t lrY = static_cast<uint16_t>(h - 1 - m);

  auto solveCalibration = [&](uint16_t ulRawX, uint16_t ulRawY, uint16_t urRawX, uint16_t urRawY,
                              uint16_t llRawX, uint16_t llRawY, uint16_t lrRawX, uint16_t lrRawY,
                              touch_input::Calibration& calibrated) -> bool {
    const int32_t horizDxX = std::abs(static_cast<int32_t>(urRawX) - static_cast<int32_t>(ulRawX)) +
                             std::abs(static_cast<int32_t>(lrRawX) - static_cast<int32_t>(llRawX));
    const int32_t horizDxY = std::abs(static_cast<int32_t>(urRawY) - static_cast<int32_t>(ulRawY)) +
                             std::abs(static_cast<int32_t>(lrRawY) - static_cast<int32_t>(llRawY));
    const bool swapXY = horizDxY > horizDxX;

    const uint16_t srcUlX = swapXY ? ulRawY : ulRawX;
    const uint16_t srcUrX = swapXY ? urRawY : urRawX;
    const uint16_t srcLlX = swapXY ? llRawY : llRawX;
    const uint16_t srcLrX = swapXY ? lrRawY : lrRawX;
    const uint16_t srcUlY = swapXY ? ulRawX : ulRawY;
    const uint16_t srcUrY = swapXY ? urRawX : urRawY;
    const uint16_t srcLlY = swapXY ? llRawX : llRawY;
    const uint16_t srcLrY = swapXY ? lrRawX : lrRawY;

    const uint16_t minRawX = std::min(std::min(srcUlX, srcUrX), std::min(srcLlX, srcLrX));
    const uint16_t maxRawX = std::max(std::max(srcUlX, srcUrX), std::max(srcLlX, srcLrX));
    const uint16_t minRawY = std::min(std::min(srcUlY, srcUrY), std::min(srcLlY, srcLrY));
    const uint16_t maxRawY = std::max(std::max(srcUlY, srcUrY), std::max(srcLlY, srcLrY));
    const uint16_t spanX = static_cast<uint16_t>(maxRawX - minRawX);
    const uint16_t spanY = static_cast<uint16_t>(maxRawY - minRawY);
    if (spanX < 600 || spanY < 600) {
      return false;
    }

    const int32_t mPx = 24;
    const int32_t wPx = static_cast<int32_t>(AppConfig::kScreenWidth);
    const int32_t hPx = static_cast<int32_t>(AppConfig::kScreenHeight);
    const int32_t innerW = wPx - 1 - 2 * mPx;
    const int32_t innerH = hPx - 1 - 2 * mPx;
    if (innerW <= 0 || innerH <= 0) {
      return false;
    }

    const int32_t rawSpanX = static_cast<int32_t>(maxRawX) - static_cast<int32_t>(minRawX);
    const int32_t rawSpanY = static_cast<int32_t>(maxRawY) - static_cast<int32_t>(minRawY);
    const int32_t edgePadRawX = (rawSpanX * mPx) / innerW;
    const int32_t edgePadRawY = (rawSpanY * mPx) / innerH;

    int32_t effectiveMinX = static_cast<int32_t>(minRawX) - edgePadRawX;
    int32_t effectiveMaxX = static_cast<int32_t>(maxRawX) + edgePadRawX;
    int32_t effectiveMinY = static_cast<int32_t>(minRawY) - edgePadRawY;
    int32_t effectiveMaxY = static_cast<int32_t>(maxRawY) + edgePadRawY;

    effectiveMinX = std::max<int32_t>(0, effectiveMinX);
    effectiveMinY = std::max<int32_t>(0, effectiveMinY);
    effectiveMaxX = std::min<int32_t>(4095, effectiveMaxX);
    effectiveMaxY = std::min<int32_t>(4095, effectiveMaxY);

    calibrated.rawMinX = static_cast<uint16_t>(effectiveMinX);
    calibrated.rawMaxX = static_cast<uint16_t>(effectiveMaxX);
    calibrated.rawMinY = static_cast<uint16_t>(effectiveMinY);
    calibrated.rawMaxY = static_cast<uint16_t>(effectiveMaxY);
    calibrated.swapXY = swapXY;
    const uint32_t leftAvgX = (static_cast<uint32_t>(srcUlX) + srcLlX) / 2U;
    const uint32_t rightAvgX = (static_cast<uint32_t>(srcUrX) + srcLrX) / 2U;
    const uint32_t topAvgY = (static_cast<uint32_t>(srcUlY) + srcUrY) / 2U;
    const uint32_t bottomAvgY = (static_cast<uint32_t>(srcLlY) + srcLrY) / 2U;
    calibrated.invertX = leftAvgX > rightAvgX;
    calibrated.invertY = topAvgY > bottomAvgY;
    calibrated.xCorrLeft = 0;
    calibrated.xCorrRight = 0;
    calibrated.yCorr = 0;
    return true;
  };

  touch_input::Calibration pass1Cal = {};
  touch_input::Calibration pass2Cal = {};
  bool pass1Ok = false;
  bool pass2Ok = false;

  for (int pass = 1; pass <= 2; ++pass) {
    const bool requireNearTarget = (pass == 2);
    bool passSolved = false;
    for (int attempt = 1; attempt <= 2; ++attempt) {
      uint16_t ulRawX = 0, ulRawY = 0;
      uint16_t urRawX = 0, urRawY = 0;
      uint16_t llRawX = 0, llRawY = 0;
      uint16_t lrRawX = 0, lrRawY = 0;
      if (!captureCalibrationPoint(ulX, ulY, ulRawX, ulRawY, requireNearTarget) ||
          !captureCalibrationPoint(urX, urY, urRawX, urRawY, requireNearTarget) ||
          !captureCalibrationPoint(llX, llY, llRawX, llRawY, requireNearTarget) ||
          !captureCalibrationPoint(lrX, lrY, lrRawX, lrRawY, requireNearTarget)) {
        ESP_LOGE(kTouchTag, "cal capture timeout pass=%d attempt=%d", pass, attempt);
        continue;
      }
      ESP_LOGI(kTouchTag,
               "cal raw pass=%d attempt=%d UL=(%u,%u) UR=(%u,%u) LL=(%u,%u) LR=(%u,%u)", pass,
               attempt, ulRawX, ulRawY, urRawX, urRawY, llRawX, llRawY, lrRawX, lrRawY);

      touch_input::Calibration solved = {};
      if (!solveCalibration(ulRawX, ulRawY, urRawX, urRawY, llRawX, llRawY, lrRawX, lrRawY,
                            solved)) {
        ESP_LOGE(kTouchTag, "cal spans invalid pass=%d attempt=%d", pass, attempt);
        continue;
      }

      if (pass == 1) {
        pass1Cal = solved;
        pass1Ok = true;
        touch_input::setCalibration(pass1Cal);
        ESP_LOGI(kTouchTag,
                 "cal pass1 solved minX=%u maxX=%u minY=%u maxY=%u swap=%d invX=%d invY=%d",
                 pass1Cal.rawMinX, pass1Cal.rawMaxX, pass1Cal.rawMinY, pass1Cal.rawMaxY,
                 pass1Cal.swapXY ? 1 : 0, pass1Cal.invertX ? 1 : 0, pass1Cal.invertY ? 1 : 0);
      } else {
        pass2Cal = solved;
        pass2Cal.xCorrLeft = 0;
        pass2Cal.xCorrRight = 0;
        pass2Cal.yCorr = 0;
        pass2Ok = true;
        ESP_LOGI(kTouchTag,
                 "cal pass2 solved minX=%u maxX=%u minY=%u maxY=%u swap=%d invX=%d invY=%d "
                 "xCorrL=%d xCorrR=%d yCorr=%d",
                 pass2Cal.rawMinX, pass2Cal.rawMaxX, pass2Cal.rawMinY, pass2Cal.rawMaxY,
                 pass2Cal.swapXY ? 1 : 0, pass2Cal.invertX ? 1 : 0, pass2Cal.invertY ? 1 : 0,
                 static_cast<int>(pass2Cal.xCorrLeft), static_cast<int>(pass2Cal.xCorrRight),
                 static_cast<int>(pass2Cal.yCorr));
      }
      passSolved = true;
      break;
    }
    if (!passSolved) {
      ESP_LOGE(kTouchTag, "calibration pass %d failed", pass);
      if (pass == 1) {
        break;
      }
    }
  }

  if (pass2Ok) {
    touch_input::setCalibration(pass2Cal);
    if (!touch_input::saveCalibration(pass2Cal)) {
      ESP_LOGW(kTouchTag, "failed to persist pass2 calibration");
    }
    ESP_LOGI(kTouchTag,
             "cal saved pass2 minX=%u maxX=%u minY=%u maxY=%u swap=%d invX=%d invY=%d",
             pass2Cal.rawMinX, pass2Cal.rawMaxX, pass2Cal.rawMinY, pass2Cal.rawMaxY,
             pass2Cal.swapXY ? 1 : 0, pass2Cal.invertX ? 1 : 0, pass2Cal.invertY ? 1 : 0);
    showPostCalibrationColorCheck();
    (void)display_spi::clear(0x0000);
    return true;
  }
  if (pass1Ok) {
    touch_input::setCalibration(pass1Cal);
    if (!touch_input::saveCalibration(pass1Cal)) {
      ESP_LOGW(kTouchTag, "failed to persist pass1 calibration fallback");
    }
    ESP_LOGW(kTouchTag,
             "cal saved pass1 fallback minX=%u maxX=%u minY=%u maxY=%u swap=%d invX=%d invY=%d",
             pass1Cal.rawMinX, pass1Cal.rawMaxX, pass1Cal.rawMinY, pass1Cal.rawMaxY,
             pass1Cal.swapXY ? 1 : 0, pass1Cal.invertX ? 1 : 0, pass1Cal.invertY ? 1 : 0);
    showPostCalibrationColorCheck();
    (void)display_spi::clear(0x0000);
    return true;
  }

  ESP_LOGE(kTouchTag, "calibration failed after retries; using defaults");
  return false;
}
