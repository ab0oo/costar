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
}  // namespace

namespace display_bootstrap {

void initPins() {
  if (AppConfig::kBoardBlueLedPin >= 0) {
    const int ledOffLevel = AppConfig::kBoardBlueLedOffHigh ? 1 : 0;
    setOutputLevel(AppConfig::kBoardBlueLedPin, ledOffLevel, "board LED");
  }

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
