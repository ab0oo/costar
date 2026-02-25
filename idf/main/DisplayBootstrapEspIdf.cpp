#include "DisplayBootstrapEspIdf.h"

#include "AppConfig.h"

#include "driver/gpio.h"
#include "esp_log.h"

namespace {
constexpr const char* kBootTag = "boot";

void setOutputLevel(int8_t pin, int level, const char* label) {
  if (pin < 0) {
    return;
  }
  const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << gpio;
  cfg.mode = GPIO_MODE_OUTPUT;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&cfg));
  ESP_ERROR_CHECK(gpio_set_level(gpio, level));
  ESP_LOGI(kBootTag, "%s pin %d set %s", label, static_cast<int>(pin), level ? "HIGH" : "LOW");
}

void setLedOff(int8_t pin, bool offHigh, const char* label) {
  if (pin < 0) {
    return;
  }
  const int offLevel = offHigh ? 1 : 0;
  setOutputLevel(pin, offLevel, label);
}

void warnIfLedSharesCriticalPin(int8_t ledPin, const char* ledLabel) {
  if (ledPin < 0) {
    return;
  }
  if (ledPin == AppConfig::kTftDcPin || ledPin == AppConfig::kTftCsPin || ledPin == AppConfig::kTftSclkPin ||
      ledPin == AppConfig::kTftMosiPin || ledPin == AppConfig::kTftMisoPin || ledPin == AppConfig::kTftRstPin) {
    ESP_LOGW(kBootTag, "%s pin %d shares TFT bus signal; it may glow/toggle during display updates", ledLabel,
             static_cast<int>(ledPin));
  }
}
}  // namespace

namespace display_bootstrap {

void initPins() {
  // Force all configured indicator LEDs off before any peripheral setup.
  setLedOff(AppConfig::kBoardBlueLedPin, AppConfig::kBoardBlueLedOffHigh, "board LED");
  setLedOff(AppConfig::kDiagnosticLedPin, false, "diagnostic LED");
  warnIfLedSharesCriticalPin(AppConfig::kBoardBlueLedPin, "board LED");
  warnIfLedSharesCriticalPin(AppConfig::kDiagnosticLedPin, "diagnostic LED");

  if (AppConfig::kTouchEnabled && AppConfig::kTouchCsPin >= 0) {
    setOutputLevel(AppConfig::kTouchCsPin, 1, "touch CS");
  }

  if (AppConfig::kSdCsPin >= 0) {
    setOutputLevel(AppConfig::kSdCsPin, 1, "SD CS");
  }

  const int backlightLevel = AppConfig::kBacklightOnHigh ? 1 : 0;
  setOutputLevel(AppConfig::kBacklightPin, backlightLevel, "backlight");
}

}  // namespace display_bootstrap
