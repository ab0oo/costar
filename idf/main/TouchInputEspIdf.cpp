#include "TouchInputEspIdf.h"

#include "AppConfig.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"

#include <algorithm>
#include <cstdint>

namespace {
constexpr const char* kTag = "touch";
constexpr spi_host_device_t kTouchHost = SPI2_HOST;
constexpr int kDmaChannel = SPI_DMA_CH_AUTO;
constexpr int kTouchClockHz = 2500000;
// Keep threshold slightly lower than Arduino library during porting so weak
// presses are still captured while we validate mapping.
constexpr int kTouchZThreshold = 180;

spi_device_handle_t sTouchDevice = nullptr;
bool sBusInitialized = false;
uint32_t sNoIrqPollCounter = 0;

int32_t clampi(int32_t value, int32_t minValue, int32_t maxValue) {
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return value;
}

int32_t mapLinear(int32_t x, int32_t inMin, int32_t inMax, int32_t outMin, int32_t outMax) {
  if (inMax == inMin) {
    return outMin;
  }
  const int64_t num = static_cast<int64_t>(x - inMin) * static_cast<int64_t>(outMax - outMin);
  const int64_t den = static_cast<int64_t>(inMax - inMin);
  return static_cast<int32_t>(num / den + outMin);
}

bool isPressedByIrq() {
  if (AppConfig::kTouchIrqPin < 0) {
    return true;
  }
  const int level = gpio_get_level(static_cast<gpio_num_t>(AppConfig::kTouchIrqPin));
  return level == 0;
}

bool readAxis(uint8_t cmd, uint16_t& out) {
  if (sTouchDevice == nullptr) {
    return false;
  }
  uint8_t tx[3] = {cmd, 0x00, 0x00};
  uint8_t rx[3] = {};
  spi_transaction_t t = {};
  t.length = 24;
  t.tx_buffer = tx;
  t.rx_buffer = rx;
  if (spi_device_polling_transmit(sTouchDevice, &t) != ESP_OK) {
    return false;
  }
  const uint16_t raw12 = static_cast<uint16_t>((rx[1] << 8) | rx[2]) >> 3;
  out = raw12;
  return true;
}

uint16_t bestTwoAvg(uint16_t a, uint16_t b, uint16_t c) {
  const uint16_t ab = (a > b) ? static_cast<uint16_t>(a - b) : static_cast<uint16_t>(b - a);
  const uint16_t ac = (a > c) ? static_cast<uint16_t>(a - c) : static_cast<uint16_t>(c - a);
  const uint16_t bc = (b > c) ? static_cast<uint16_t>(b - c) : static_cast<uint16_t>(c - b);
  if (ab <= ac && ab <= bc) {
    return static_cast<uint16_t>((a + b) >> 1);
  }
  if (ac <= ab && ac <= bc) {
    return static_cast<uint16_t>((a + c) >> 1);
  }
  return static_cast<uint16_t>((b + c) >> 1);
}

bool readRawStable(uint16_t& rawX, uint16_t& rawY, uint16_t& zOut) {
  // Emulate XPT2046_Touchscreen::update() sampling order so calibration/mapping
  // behaves like the Arduino path.
  uint16_t z1 = 0;
  uint16_t z2 = 0;
  if (!readAxis(0xB1, z1) || !readAxis(0xC1, z2)) {
    return false;
  }
  int z = static_cast<int>(z1) + 4095 - static_cast<int>(z2);
  if (z < 0) {
    z = 0;
  }
  if (z < kTouchZThreshold) {
    return false;
  }

  uint16_t dummy = 0;
  uint16_t data0 = 0;
  uint16_t data1 = 0;
  uint16_t data2 = 0;
  uint16_t data3 = 0;
  uint16_t data4 = 0;
  uint16_t data5 = 0;
  (void)readAxis(0x91, dummy);  // first X read is typically noisy
  if (!readAxis(0xD1, data0) || !readAxis(0x91, data1) || !readAxis(0xD1, data2) ||
      !readAxis(0x91, data3) || !readAxis(0xD0, data4) || !readAxis(0x00, data5)) {
    return false;
  }

  // These names intentionally follow the upstream algorithm.
  const uint16_t x = bestTwoAvg(data0, data2, data4);
  const uint16_t y = bestTwoAvg(data1, data3, data5);

  // Apply the same touch rotation as Arduino path (setRotation(2)).
  rawX = y;
  rawY = static_cast<uint16_t>(4095U - x);
  zOut = static_cast<uint16_t>(z);
  return true;
}
}  // namespace

namespace touch_input {

bool init() {
  if (sTouchDevice != nullptr) {
    return true;
  }

  if (AppConfig::kTouchIrqPin >= 0) {
    gpio_config_t irqCfg = {};
    irqCfg.pin_bit_mask = 1ULL << static_cast<gpio_num_t>(AppConfig::kTouchIrqPin);
    irqCfg.mode = GPIO_MODE_INPUT;
    irqCfg.pull_up_en = GPIO_PULLUP_DISABLE;
    irqCfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    irqCfg.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&irqCfg) != ESP_OK) {
      ESP_LOGW(kTag, "irq pin config failed pin=%d", AppConfig::kTouchIrqPin);
    }
  }

  spi_bus_config_t busCfg = {};
  busCfg.sclk_io_num = AppConfig::kTouchSpiSckPin;
  busCfg.mosi_io_num = AppConfig::kTouchSpiMosiPin;
  busCfg.miso_io_num = AppConfig::kTouchSpiMisoPin;
  busCfg.quadwp_io_num = -1;
  busCfg.quadhd_io_num = -1;
  busCfg.max_transfer_sz = 8;

  if (!sBusInitialized) {
    const esp_err_t busErr = spi_bus_initialize(kTouchHost, &busCfg, kDmaChannel);
    if (busErr != ESP_OK) {
      ESP_LOGE(kTag, "spi bus init failed err=0x%x", static_cast<unsigned>(busErr));
      return false;
    }
    sBusInitialized = true;
    ESP_LOGI(kTag, "spi bus ready host=%d sclk=%d mosi=%d miso=%d", static_cast<int>(kTouchHost),
             AppConfig::kTouchSpiSckPin, AppConfig::kTouchSpiMosiPin, AppConfig::kTouchSpiMisoPin);
  }

  spi_device_interface_config_t devCfg = {};
  devCfg.clock_speed_hz = kTouchClockHz;
  devCfg.mode = 0;
  devCfg.spics_io_num = AppConfig::kTouchCsPin;
  devCfg.queue_size = 1;

  const esp_err_t devErr = spi_bus_add_device(kTouchHost, &devCfg, &sTouchDevice);
  if (devErr != ESP_OK) {
    ESP_LOGE(kTag, "spi add device failed err=0x%x", static_cast<unsigned>(devErr));
    return false;
  }

  ESP_LOGI(kTag, "device ready cs=%d irq=%d hz=%d", AppConfig::kTouchCsPin, AppConfig::kTouchIrqPin,
           devCfg.clock_speed_hz);
  return true;
}

bool read(Point& out) {
  if (!init()) {
    return false;
  }
  const bool irqPressed = isPressedByIrq();
  if (!irqPressed) {
    // On some boards IRQ wiring/noise can miss presses; poll periodically anyway
    // and rely on pressure threshold filtering from the controller data.
    ++sNoIrqPollCounter;
    if ((sNoIrqPollCounter & 0x3U) != 0U) {
      return false;
    }
  }

  uint16_t rawX = 0;
  uint16_t rawY = 0;
  uint16_t z = 0;
  if (!readRawStable(rawX, rawY, z)) {
    return false;
  }

  // Match Arduino TouchMapper exactly:
  // y <- raw.x (min->max), x <- raw.y (max->min).
  int32_t y = mapLinear(rawX, AppConfig::kTouchRawMinX, AppConfig::kTouchRawMaxX, 0,
                        static_cast<int32_t>(AppConfig::kScreenHeight) - 1);
  int32_t x = mapLinear(rawY, AppConfig::kTouchRawMaxY, AppConfig::kTouchRawMinY, 0,
                        static_cast<int32_t>(AppConfig::kScreenWidth) - 1);

  x = clampi(x, 0, static_cast<int32_t>(AppConfig::kScreenWidth) - 1);
  y = clampi(y, 0, static_cast<int32_t>(AppConfig::kScreenHeight) - 1);

  if (AppConfig::kTouchInvertX) {
    x = static_cast<int32_t>(AppConfig::kScreenWidth) - 1 - x;
  }
  if (AppConfig::kTouchInvertY) {
    y = static_cast<int32_t>(AppConfig::kScreenHeight) - 1 - y;
  }

  out.rawX = rawX;
  out.rawY = rawY;
  out.x = static_cast<uint16_t>(x);
  out.y = static_cast<uint16_t>(y);
  return true;
}

}  // namespace touch_input
