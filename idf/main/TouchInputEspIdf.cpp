#include "TouchInputEspIdf.h"

#include "AppConfig.h"
#include "platform/Prefs.h"

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
// Keep threshold slightly lower so weak presses are still captured.
constexpr int kTouchZThreshold = 180;
constexpr bool kEnableRuntimeWarpCorrection = false;
constexpr const char* kTouchPrefsNs = "touch";
constexpr const char* kTouchCalValidKey = "cal_ok";
constexpr const char* kTouchMinXKey = "min_x";
constexpr const char* kTouchMaxXKey = "max_x";
constexpr const char* kTouchMinYKey = "min_y";
constexpr const char* kTouchMaxYKey = "max_y";
constexpr const char* kTouchSwapXYKey = "sw_xy";
constexpr const char* kTouchInvXKey = "inv_x";
constexpr const char* kTouchInvYKey = "inv_y";
constexpr const char* kTouchXCorrLKey = "xcor_l";
constexpr const char* kTouchXCorrRKey = "xcor_r";
constexpr const char* kTouchYCorrKey = "ycor";

spi_device_handle_t sTouchDevice = nullptr;
bool sBusInitialized = false;
bool sCalibrationInitialized = false;
bool sCalibrationPresent = false;
touch_input::Calibration sCalibration = {};
bool sTouchWasPressed = false;
int32_t sFilteredX = 0;
int32_t sFilteredY = 0;

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
  // Match the established XPT2046 sampling order so calibration remains stable.
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

  // Apply touchscreen rotation equivalent to setRotation(2).
  rawX = y;
  rawY = static_cast<uint16_t>(4095U - x);
  zOut = static_cast<uint16_t>(z);
  return true;
}

void initCalibrationDefaults() {
  if (sCalibrationInitialized) {
    return;
  }
  sCalibration.rawMinX = AppConfig::kTouchRawMinX;
  sCalibration.rawMaxX = AppConfig::kTouchRawMaxX;
  sCalibration.rawMinY = AppConfig::kTouchRawMinY;
  sCalibration.rawMaxY = AppConfig::kTouchRawMaxY;
  // Map screen X from rawY and screen Y from rawX.
  sCalibration.swapXY = true;
  sCalibration.invertX = AppConfig::kTouchInvertX;
  sCalibration.invertY = AppConfig::kTouchInvertY;
  sCalibration.xCorrLeft = 0;
  sCalibration.xCorrRight = 0;
  sCalibration.yCorr = 0;
  sCalibrationPresent = false;
  sCalibrationInitialized = true;
}
}  // namespace

namespace touch_input {

bool init() {
  initCalibrationDefaults();
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
  initCalibrationDefaults();
  if (!init()) {
    return false;
  }
  // IRQ on CYD-class boards can be noisy or unreliable depending on wiring and
  // panel variant. Treat it as advisory only and always attempt a real sample
  // so short taps are not dropped.
  (void)isPressedByIrq();

  uint16_t rawX = 0;
  uint16_t rawY = 0;
  uint16_t z = 0;
  if (!readRawStable(rawX, rawY, z)) {
    sTouchWasPressed = false;
    return false;
  }

  const int32_t sourceForX = sCalibration.swapXY ? static_cast<int32_t>(rawY)
                                                 : static_cast<int32_t>(rawX);
  const int32_t sourceForY = sCalibration.swapXY ? static_cast<int32_t>(rawX)
                                                 : static_cast<int32_t>(rawY);
  int32_t x = mapLinear(sourceForX, sCalibration.rawMinX, sCalibration.rawMaxX, 0,
                        static_cast<int32_t>(AppConfig::kScreenWidth) - 1);
  int32_t y = mapLinear(sourceForY, sCalibration.rawMinY, sCalibration.rawMaxY, 0,
                        static_cast<int32_t>(AppConfig::kScreenHeight) - 1);

  x = clampi(x, 0, static_cast<int32_t>(AppConfig::kScreenWidth) - 1);
  y = clampi(y, 0, static_cast<int32_t>(AppConfig::kScreenHeight) - 1);

  if (sCalibration.invertX) {
    x = static_cast<int32_t>(AppConfig::kScreenWidth) - 1 - x;
  }
  if (sCalibration.invertY) {
    y = static_cast<int32_t>(AppConfig::kScreenHeight) - 1 - y;
  }

  // Linear horizontal dewarp: apply stronger correction near left edge and
  // taper toward right edge (or vice versa) based on calibration refinement.
  if (kEnableRuntimeWarpCorrection) {
    const int32_t w1 = static_cast<int32_t>(AppConfig::kScreenWidth) - 1;
    if (w1 > 0) {
      const int32_t corr =
          ((w1 - x) * static_cast<int32_t>(sCalibration.xCorrLeft) +
           x * static_cast<int32_t>(sCalibration.xCorrRight)) /
          w1;
      x += corr;
    }
    y += static_cast<int32_t>(sCalibration.yCorr);
  }
  x = clampi(x, 0, static_cast<int32_t>(AppConfig::kScreenWidth) - 1);
  y = clampi(y, 0, static_cast<int32_t>(AppConfig::kScreenHeight) - 1);

  if (!sTouchWasPressed) {
    sFilteredX = x;
    sFilteredY = y;
    sTouchWasPressed = true;
  } else {
    // Keep pointer stable for dense targets (Wi-Fi rows, keyboard keys).
    sFilteredX = (sFilteredX * 3 + x) / 4;
    sFilteredY = (sFilteredY * 3 + y) / 4;
  }
  sFilteredX = clampi(sFilteredX, 0, static_cast<int32_t>(AppConfig::kScreenWidth) - 1);
  sFilteredY = clampi(sFilteredY, 0, static_cast<int32_t>(AppConfig::kScreenHeight) - 1);

  out.rawX = rawX;
  out.rawY = rawY;
  out.x = static_cast<uint16_t>(sFilteredX);
  out.y = static_cast<uint16_t>(sFilteredY);
  return true;
}

bool hasCalibration() {
  initCalibrationDefaults();
  return sCalibrationPresent;
}

bool loadCalibration(Calibration& out) {
  initCalibrationDefaults();
  const bool valid = platform::prefs::getBool(kTouchPrefsNs, kTouchCalValidKey, false);
  if (!valid) {
    out = sCalibration;
    sCalibrationPresent = false;
    return false;
  }

  Calibration loaded = {};
  loaded.rawMinX = static_cast<uint16_t>(
      platform::prefs::getUInt(kTouchPrefsNs, kTouchMinXKey, AppConfig::kTouchRawMinX));
  loaded.rawMaxX = static_cast<uint16_t>(
      platform::prefs::getUInt(kTouchPrefsNs, kTouchMaxXKey, AppConfig::kTouchRawMaxX));
  loaded.rawMinY = static_cast<uint16_t>(
      platform::prefs::getUInt(kTouchPrefsNs, kTouchMinYKey, AppConfig::kTouchRawMinY));
  loaded.rawMaxY = static_cast<uint16_t>(
      platform::prefs::getUInt(kTouchPrefsNs, kTouchMaxYKey, AppConfig::kTouchRawMaxY));
  loaded.swapXY = platform::prefs::getBool(kTouchPrefsNs, kTouchSwapXYKey, true);
  loaded.invertX = platform::prefs::getBool(kTouchPrefsNs, kTouchInvXKey, AppConfig::kTouchInvertX);
  loaded.invertY = platform::prefs::getBool(kTouchPrefsNs, kTouchInvYKey, AppConfig::kTouchInvertY);
  loaded.xCorrLeft =
      static_cast<int16_t>(platform::prefs::getInt(kTouchPrefsNs, kTouchXCorrLKey, 0));
  loaded.xCorrRight =
      static_cast<int16_t>(platform::prefs::getInt(kTouchPrefsNs, kTouchXCorrRKey, 0));
  loaded.yCorr = static_cast<int16_t>(platform::prefs::getInt(kTouchPrefsNs, kTouchYCorrKey, 0));

  // Basic sanity check so bad values cannot brick touch.
  if (loaded.rawMaxX <= loaded.rawMinX + 50 || loaded.rawMaxY <= loaded.rawMinY + 50) {
    ESP_LOGW(kTag, "invalid persisted calibration; using defaults");
    out = sCalibration;
    sCalibrationPresent = false;
    return false;
  }

  sCalibration = loaded;
  sCalibrationPresent = true;
  out = sCalibration;
  return true;
}

bool saveCalibration(const Calibration& calibration) {
  initCalibrationDefaults();
  if (calibration.rawMaxX <= calibration.rawMinX + 50 ||
      calibration.rawMaxY <= calibration.rawMinY + 50) {
    return false;
  }

  bool ok = true;
  ok = ok && platform::prefs::putUInt(kTouchPrefsNs, kTouchMinXKey, calibration.rawMinX);
  ok = ok && platform::prefs::putUInt(kTouchPrefsNs, kTouchMaxXKey, calibration.rawMaxX);
  ok = ok && platform::prefs::putUInt(kTouchPrefsNs, kTouchMinYKey, calibration.rawMinY);
  ok = ok && platform::prefs::putUInt(kTouchPrefsNs, kTouchMaxYKey, calibration.rawMaxY);
  ok = ok && platform::prefs::putBool(kTouchPrefsNs, kTouchSwapXYKey, calibration.swapXY);
  ok = ok && platform::prefs::putBool(kTouchPrefsNs, kTouchInvXKey, calibration.invertX);
  ok = ok && platform::prefs::putBool(kTouchPrefsNs, kTouchInvYKey, calibration.invertY);
  ok = ok && platform::prefs::putInt(kTouchPrefsNs, kTouchXCorrLKey, calibration.xCorrLeft);
  ok = ok && platform::prefs::putInt(kTouchPrefsNs, kTouchXCorrRKey, calibration.xCorrRight);
  ok = ok && platform::prefs::putInt(kTouchPrefsNs, kTouchYCorrKey, calibration.yCorr);
  ok = ok && platform::prefs::putBool(kTouchPrefsNs, kTouchCalValidKey, true);
  if (!ok) {
    return false;
  }

  sCalibration = calibration;
  sCalibrationPresent = true;
  return true;
}

void setCalibration(const Calibration& calibration) {
  initCalibrationDefaults();
  if (calibration.rawMaxX <= calibration.rawMinX + 50 ||
      calibration.rawMaxY <= calibration.rawMinY + 50) {
    return;
  }
  sCalibration = calibration;
  sCalibrationPresent = true;
}

void getCalibration(Calibration& out) {
  initCalibrationDefaults();
  out = sCalibration;
}

}  // namespace touch_input
