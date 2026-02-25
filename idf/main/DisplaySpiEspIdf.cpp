#include "DisplaySpiEspIdf.h"

#include "AppConfig.h"
#include "platform/Prefs.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <climits>
#include <cstddef>
#include <cstdint>

namespace {
constexpr const char* kTag = "tft";
constexpr spi_host_device_t kTftHost = SPI3_HOST;
constexpr int kDmaChannel = SPI_DMA_CH_AUTO;
constexpr int kPanelClockHz = 40 * 1000 * 1000;
constexpr uint32_t kDmaChunkRows = 16;

spi_device_handle_t sTftDevice = nullptr;
bool sBusInitialized = false;
bool sPanelInitialized = false;

static_assert(AppConfig::kPanelWidth <= UINT16_MAX, "panel width must fit 16-bit column addressing");
static_assert(AppConfig::kPanelHeight <= UINT16_MAX,
              "panel height must fit 16-bit row addressing");

uint8_t rotation() { return static_cast<uint8_t>(AppConfig::kRotation & 0x03U); }

uint16_t logicalWidth() {
  return (rotation() & 0x01U) ? AppConfig::kPanelHeight : AppConfig::kPanelWidth;
}

uint16_t logicalHeight() {
  return (rotation() & 0x01U) ? AppConfig::kPanelWidth : AppConfig::kPanelHeight;
}

void delayMs(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

bool setGpioOutput(int8_t pin, int level) {
  if (pin < 0) {
    return false;
  }
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << static_cast<gpio_num_t>(pin);
  cfg.mode = GPIO_MODE_OUTPUT;
  cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  if (gpio_config(&cfg) != ESP_OK) {
    return false;
  }
  return gpio_set_level(static_cast<gpio_num_t>(pin), level) == ESP_OK;
}

bool writeSpiBytes(int dcLevel, const uint8_t* data, size_t size) {
  if (sTftDevice == nullptr || data == nullptr || size == 0) {
    return false;
  }
  if (AppConfig::kTftDcPin >= 0) {
    if (gpio_set_level(static_cast<gpio_num_t>(AppConfig::kTftDcPin), dcLevel) != ESP_OK) {
      return false;
    }
  }

  spi_transaction_t t = {};
  t.length = size * 8U;
  t.tx_buffer = data;
  const esp_err_t err = spi_device_polling_transmit(sTftDevice, &t);
  return err == ESP_OK;
}

bool writeCommand(uint8_t cmd) { return writeSpiBytes(0, &cmd, 1); }

bool writeData(const uint8_t* data, size_t size) { return writeSpiBytes(1, data, size); }

bool writeReg(uint8_t cmd, const uint8_t* data, size_t size) {
  if (!writeCommand(cmd)) {
    return false;
  }
  if (size == 0) {
    return true;
  }
  return writeData(data, size);
}

bool resetPanel() {
  if (AppConfig::kTftRstPin < 0) {
    return true;
  }
  if (AppConfig::kTftRstPin == AppConfig::kTftMisoPin) {
    ESP_LOGW(kTag, "skip reset toggle: rst pin shares TFT MISO (%d)", AppConfig::kTftRstPin);
    return true;
  }
  if (!setGpioOutput(AppConfig::kTftRstPin, 1)) {
    ESP_LOGW(kTag, "panel reset gpio setup failed pin=%d", AppConfig::kTftRstPin);
    return false;
  }
  gpio_set_level(static_cast<gpio_num_t>(AppConfig::kTftRstPin), 0);
  delayMs(20);
  gpio_set_level(static_cast<gpio_num_t>(AppConfig::kTftRstPin), 1);
  delayMs(120);
  return true;
}

bool runIli9341Init() {
  // Mirror TFT_eSPI ILI9341_2_DRIVER init sequence byte-for-byte.
  static constexpr uint8_t kCf[] = {0x00, 0xC1, 0x30};
  static constexpr uint8_t kEd[] = {0x64, 0x03, 0x12, 0x81};
  static constexpr uint8_t kE8[] = {0x85, 0x00, 0x78};
  static constexpr uint8_t kCb[] = {0x39, 0x2C, 0x00, 0x34, 0x02};
  static constexpr uint8_t kF7[] = {0x20};
  static constexpr uint8_t kEa[] = {0x00, 0x00};
  static constexpr uint8_t kC0[] = {0x10};
  static constexpr uint8_t kC1[] = {0x00};
  static constexpr uint8_t kC5[] = {0x30, 0x30};
  static constexpr uint8_t kC7[] = {0xB7};
  static constexpr uint8_t k3A[] = {0x55};  // RGB565
  static constexpr uint8_t k36[] = {0x08};  // default MADCTL in TFT_eSPI init table
  static constexpr uint8_t kB1[] = {0x00, 0x1A};
  static constexpr uint8_t kB6[] = {0x08, 0x82, 0x27};
  static constexpr uint8_t kF2[] = {0x00};
  static constexpr uint8_t k26[] = {0x01};
  static constexpr uint8_t kE0[] = {0x0F, 0x2A, 0x28, 0x08, 0x0E, 0x08, 0x54, 0xA9,
                                    0x43, 0x0A, 0x0F, 0x00, 0x00, 0x00, 0x00};
  static constexpr uint8_t kE1[] = {0x00, 0x15, 0x17, 0x07, 0x11, 0x06, 0x2B, 0x56,
                                    0x3C, 0x05, 0x10, 0x0F, 0x3F, 0x3F, 0x0F};
  static constexpr uint8_t k2B[] = {0x00, 0x00, 0x01, 0x3F};
  static constexpr uint8_t k2A[] = {0x00, 0x00, 0x00, 0xEF};

  if (!writeReg(0xCF, kCf, sizeof(kCf)) || !writeReg(0xED, kEd, sizeof(kEd)) ||
      !writeReg(0xE8, kE8, sizeof(kE8)) || !writeReg(0xCB, kCb, sizeof(kCb)) ||
      !writeReg(0xF7, kF7, sizeof(kF7)) || !writeReg(0xEA, kEa, sizeof(kEa)) ||
      !writeReg(0xC0, kC0, sizeof(kC0)) || !writeReg(0xC1, kC1, sizeof(kC1)) ||
      !writeReg(0xC5, kC5, sizeof(kC5)) || !writeReg(0xC7, kC7, sizeof(kC7)) ||
      !writeReg(0x3A, k3A, sizeof(k3A)) || !writeReg(0x36, k36, sizeof(k36)) ||
      !writeReg(0xB1, kB1, sizeof(kB1)) || !writeReg(0xB6, kB6, sizeof(kB6)) ||
      !writeReg(0xF2, kF2, sizeof(kF2)) || !writeReg(0x26, k26, sizeof(k26)) ||
      !writeReg(0xE0, kE0, sizeof(kE0)) || !writeReg(0xE1, kE1, sizeof(kE1)) ||
      !writeReg(0x2B, k2B, sizeof(k2B)) || !writeReg(0x2A, k2A, sizeof(k2A))) {
    return false;
  }

  if (!writeCommand(0x11)) {
    return false;
  }  // SLPOUT
  delayMs(120);
  if (!writeCommand(0x29)) {
    return false;
  }  // DISPON
  delayMs(20);
  return true;
}

uint8_t madctlForRotationAndOrder(uint8_t rot, bool bgr) {
  uint8_t value = 0x20;  // rotation=1
  switch (rot & 0x03U) {
    case 0: value = 0x40; break;
    case 1: value = 0x20; break;
    case 2: value = 0x80; break;
    case 3: value = 0xE0; break;
    default: break;
  }
  if (bgr) {
    value |= 0x08;
  }
  return value;
}

bool applyPanelRuntimeTuning() {
  constexpr const char* kDisplayPrefsNs = "display";
  constexpr const char* kColorSetKey = "color_set";
  constexpr const char* kColorBgrKey = "color_bgr";
  constexpr const char* kInvertSetKey = "inv_set";
  constexpr const char* kInvertOnKey = "inv_on";

  const bool hasColorSetting = platform::prefs::getBool(kDisplayPrefsNs, kColorSetKey, false);
  const bool useBgr =
      hasColorSetting ? platform::prefs::getBool(kDisplayPrefsNs, kColorBgrKey, false) : false;
  const bool hasInvertSetting = platform::prefs::getBool(kDisplayPrefsNs, kInvertSetKey, false);
  const bool storedInvert =
      hasInvertSetting ? platform::prefs::getBool(kDisplayPrefsNs, kInvertOnKey, false) : false;
  const bool useInvert = hasInvertSetting ? storedInvert : true;

  const uint8_t madctl = madctlForRotationAndOrder(rotation(), useBgr);
  if (!writeReg(0x36, &madctl, 1U)) {
    return false;
  }
  if (!writeCommand(useInvert ? 0x21 : 0x20)) {
    return false;
  }

  ESP_LOGI(kTag,
           "panel runtime tuning rot=%u madctl=0x%02x color_set=%d bgr=%d inv_set=%d "
           "invert(stored=%d applied=%d)",
           static_cast<unsigned>(rotation()), madctl, hasColorSetting ? 1 : 0, useBgr ? 1 : 0,
           hasInvertSetting ? 1 : 0, storedInvert ? 1 : 0, useInvert ? 1 : 0);
  return true;
}

bool applyPanelRuntimeTuningExplicit(bool useBgr, bool useInvert) {
  const uint8_t madctl = madctlForRotationAndOrder(rotation(), useBgr);
  if (!writeReg(0x36, &madctl, 1U)) {
    return false;
  }
  if (!writeCommand(useInvert ? 0x21 : 0x20)) {
    return false;
  }
  ESP_LOGI(kTag, "panel explicit tuning rot=%u madctl=0x%02x bgr=%d invert=%d",
           static_cast<unsigned>(rotation()), madctl, useBgr ? 1 : 0, useInvert ? 1 : 0);
  return true;
}

bool setAddressWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  const uint8_t colData[] = {static_cast<uint8_t>(x0 >> 8), static_cast<uint8_t>(x0 & 0xFF),
                             static_cast<uint8_t>(x1 >> 8), static_cast<uint8_t>(x1 & 0xFF)};
  const uint8_t rowData[] = {static_cast<uint8_t>(y0 >> 8), static_cast<uint8_t>(y0 & 0xFF),
                             static_cast<uint8_t>(y1 >> 8), static_cast<uint8_t>(y1 & 0xFF)};
  if (!writeReg(0x2A, colData, sizeof(colData))) {
    return false;
  }
  if (!writeReg(0x2B, rowData, sizeof(rowData))) {
    return false;
  }
  return writeCommand(0x2C);  // RAMWR
}

bool fillColor565(uint16_t color, uint32_t pixelCount) {
  static constexpr size_t kChunkPixels = 2048;
  uint8_t line[kChunkPixels * 2];
  const uint8_t hi = static_cast<uint8_t>(color >> 8);
  const uint8_t lo = static_cast<uint8_t>(color & 0xFF);
  for (size_t i = 0; i < kChunkPixels; ++i) {
    line[i * 2] = hi;
    line[i * 2 + 1] = lo;
  }

  uint32_t remaining = pixelCount;
  while (remaining > 0) {
    const uint16_t now =
        remaining > kChunkPixels ? static_cast<uint16_t>(kChunkPixels) : static_cast<uint16_t>(remaining);
    if (!writeData(line, now * 2U)) {
      return false;
    }
    remaining -= now;
  }
  return true;
}
}  // namespace

namespace display_spi {

bool init() {
  if (sTftDevice != nullptr) {
    return true;
  }

  spi_bus_config_t busCfg = {};
  busCfg.mosi_io_num = AppConfig::kTftMosiPin;
  busCfg.miso_io_num = AppConfig::kTftMisoPin;
  busCfg.sclk_io_num = AppConfig::kTftSclkPin;
  busCfg.quadwp_io_num = -1;
  busCfg.quadhd_io_num = -1;
  const uint32_t maxTransferBytes =
      static_cast<uint32_t>(AppConfig::kPanelWidth) * kDmaChunkRows * 2U;
  busCfg.max_transfer_sz = static_cast<int>(maxTransferBytes);

  if (!sBusInitialized) {
    const esp_err_t busErr = spi_bus_initialize(kTftHost, &busCfg, kDmaChannel);
    if (busErr != ESP_OK) {
      ESP_LOGE(kTag, "spi bus init failed err=0x%x", static_cast<unsigned>(busErr));
      return false;
    }
    sBusInitialized = true;
    ESP_LOGI(kTag, "spi bus ready host=%d sclk=%d mosi=%d miso=%d", static_cast<int>(kTftHost),
             AppConfig::kTftSclkPin, AppConfig::kTftMosiPin, AppConfig::kTftMisoPin);
  }

  spi_device_interface_config_t devCfg = {};
  devCfg.clock_speed_hz = kPanelClockHz;
  devCfg.mode = 0;
  devCfg.spics_io_num = AppConfig::kTftCsPin;
  devCfg.queue_size = 6;
  devCfg.flags = SPI_DEVICE_NO_DUMMY;

  const esp_err_t devErr = spi_bus_add_device(kTftHost, &devCfg, &sTftDevice);
  if (devErr != ESP_OK) {
    ESP_LOGE(kTag, "spi add device failed err=0x%x", static_cast<unsigned>(devErr));
    return false;
  }

  ESP_LOGI(kTag, "panel spi device ready cs=%d dc=%d rst=%d hz=%d", AppConfig::kTftCsPin,
           AppConfig::kTftDcPin, AppConfig::kTftRstPin, devCfg.clock_speed_hz);
  return true;
}

bool initPanel() {
  if (sPanelInitialized) {
    return true;
  }
  if (!init()) {
    return false;
  }
  if (!setGpioOutput(AppConfig::kTftDcPin, 1)) {
    ESP_LOGE(kTag, "panel DC pin setup failed pin=%d", AppConfig::kTftDcPin);
    return false;
  }
  (void)resetPanel();
  if (!runIli9341Init()) {
    ESP_LOGE(kTag, "panel init command sequence failed");
    return false;
  }
  if (!applyPanelRuntimeTuning()) {
    ESP_LOGE(kTag, "panel runtime tuning failed");
    return false;
  }
  sPanelInitialized = true;
  ESP_LOGI(kTag, "panel init complete (ili9341-style sequence)");
  return true;
}

bool applyPanelTuning(bool bgr, bool invert, bool persist) {
  constexpr const char* kDisplayPrefsNs = "display";
  constexpr const char* kColorSetKey = "color_set";
  constexpr const char* kColorBgrKey = "color_bgr";
  constexpr const char* kInvertSetKey = "inv_set";
  constexpr const char* kInvertOnKey = "inv_on";

  if (!sPanelInitialized && !initPanel()) {
    return false;
  }
  if (persist) {
    (void)platform::prefs::putBool(kDisplayPrefsNs, kColorSetKey, true);
    (void)platform::prefs::putBool(kDisplayPrefsNs, kColorBgrKey, bgr);
    (void)platform::prefs::putBool(kDisplayPrefsNs, kInvertSetKey, true);
    (void)platform::prefs::putBool(kDisplayPrefsNs, kInvertOnKey, invert);
  }
  return applyPanelRuntimeTuningExplicit(bgr, invert);
}

bool drawSanityPattern() {
  if (!sPanelInitialized && !initPanel()) {
    return false;
  }

  const uint16_t panelW = logicalWidth();
  const uint16_t panelH = logicalHeight();

  if (!setAddressWindow(0, 0, panelW - 1, panelH - 1)) {
    ESP_LOGE(kTag, "set full window failed");
    return false;
  }
  if (!fillColor565(0x0000, static_cast<uint32_t>(panelW) * static_cast<uint32_t>(panelH))) {
    ESP_LOGE(kTag, "clear frame failed");
    return false;
  }

  const uint16_t barHeight = panelH / 8U;
  if (barHeight == 0) {
    ESP_LOGE(kTag, "invalid panel height=%u", panelH);
    return false;
  }
  const struct {
    uint16_t y0;
    uint16_t y1;
    uint16_t color;
  } bars[] = {
      {0, static_cast<uint16_t>(barHeight - 1), 0xF800},                         // red
      {static_cast<uint16_t>(barHeight), static_cast<uint16_t>(barHeight * 2 - 1), 0x07E0},  // green
      {static_cast<uint16_t>(barHeight * 2), static_cast<uint16_t>(barHeight * 3 - 1),
       0x001F}  // blue
  };

  for (const auto& bar : bars) {
    if (!setAddressWindow(0, bar.y0, panelW - 1, bar.y1)) {
      ESP_LOGE(kTag, "set bar window failed y0=%u y1=%u", bar.y0, bar.y1);
      return false;
    }
    const uint32_t pixels =
        static_cast<uint32_t>(panelW) * static_cast<uint32_t>(bar.y1 - bar.y0 + 1);
    if (!fillColor565(bar.color, pixels)) {
      ESP_LOGE(kTag, "bar fill failed y0=%u y1=%u", bar.y0, bar.y1);
      return false;
    }
  }

  ESP_LOGI(kTag, "sanity pattern drawn");
  return true;
}

bool fillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color565) {
  if (w == 0 || h == 0) {
    return true;
  }
  if (!sPanelInitialized && !initPanel()) {
    return false;
  }

  const uint32_t panelW = logicalWidth();
  const uint32_t panelH = logicalHeight();
  if (x >= panelW || y >= panelH) {
    return false;
  }

  uint32_t x1 = static_cast<uint32_t>(x) + static_cast<uint32_t>(w) - 1U;
  uint32_t y1 = static_cast<uint32_t>(y) + static_cast<uint32_t>(h) - 1U;
  if (x1 >= panelW) {
    x1 = panelW - 1U;
  }
  if (y1 >= panelH) {
    y1 = panelH - 1U;
  }

  const uint16_t x1u16 = static_cast<uint16_t>(x1);
  const uint16_t y1u16 = static_cast<uint16_t>(y1);
  if (!setAddressWindow(x, y, x1u16, y1u16)) {
    return false;
  }
  const uint32_t pixels =
      static_cast<uint32_t>(x1u16 - x + 1U) * static_cast<uint32_t>(y1u16 - y + 1U);
  return fillColor565(color565, pixels);
}

bool clear(uint16_t color565) {
  return fillRect(0, 0, logicalWidth(), logicalHeight(), color565);
}

bool drawRgb565(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t* pixels) {
  if (w == 0 || h == 0 || pixels == nullptr) {
    return false;
  }
  if (!sPanelInitialized && !initPanel()) {
    return false;
  }

  const uint32_t panelW = logicalWidth();
  const uint32_t panelH = logicalHeight();
  if (x >= panelW || y >= panelH) {
    return false;
  }

  uint32_t x1 = static_cast<uint32_t>(x) + static_cast<uint32_t>(w) - 1U;
  uint32_t y1 = static_cast<uint32_t>(y) + static_cast<uint32_t>(h) - 1U;
  if (x1 >= panelW) {
    x1 = panelW - 1U;
  }
  if (y1 >= panelH) {
    y1 = panelH - 1U;
  }

  const uint16_t outW = static_cast<uint16_t>(x1 - x + 1U);
  const uint16_t outH = static_cast<uint16_t>(y1 - y + 1U);
  if (!setAddressWindow(x, y, static_cast<uint16_t>(x1), static_cast<uint16_t>(y1))) {
    return false;
  }

  // Panel expects RGB565 as big-endian bytes on SPI. Keep chunks large to reduce SPI transaction overhead.
  static constexpr size_t kChunkPixels = 2048;
  static uint8_t chunk[kChunkPixels * 2];
  const uint32_t total = static_cast<uint32_t>(outW) * static_cast<uint32_t>(outH);
  uint32_t idx = 0;
  while (idx < total) {
    const uint32_t now = std::min<uint32_t>(kChunkPixels, total - idx);
    for (uint32_t i = 0; i < now; ++i) {
      const uint16_t c = pixels[idx + i];
      chunk[i * 2U] = static_cast<uint8_t>(c >> 8);
      chunk[i * 2U + 1U] = static_cast<uint8_t>(c & 0xFFU);
    }
    if (!writeData(chunk, now * 2U)) {
      return false;
    }
    idx += now;
  }
  return true;
}

uint16_t width() { return logicalWidth(); }

uint16_t height() { return logicalHeight(); }

}  // namespace display_spi
