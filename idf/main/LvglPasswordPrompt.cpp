#include "LvglPasswordPrompt.h"

#include "AppConfig.h"
#include "DisplaySpiEspIdf.h"
#include "TouchInputEspIdf.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <string>

namespace {

constexpr const char* kTag = "lvgl-prompt";
constexpr uint32_t kTickMs = 10;
constexpr uint32_t kPromptTaskStackBytes = 16384;
constexpr UBaseType_t kPromptTaskPriority = 5;

bool sLvglReady = false;
lv_display_t* sDisplay = nullptr;
lv_indev_t* sTouchInput = nullptr;
void* sBuf1 = nullptr;
void* sBuf2 = nullptr;
size_t sBufSize = 0;

struct PromptState {
  bool done = false;
  bool accepted = false;
};

struct PromptTaskCtx {
  std::string title;
  std::string subtitle;
  std::string* outPassword = nullptr;
  bool result = false;
  TaskHandle_t caller = nullptr;
};

void flushCb(lv_display_t* display, const lv_area_t* area, uint8_t* pxMap) {
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

void touchReadCb(lv_indev_t* indev, lv_indev_data_t* data) {
  (void)indev;
  touch_input::Point p;
  if (touch_input::read(p)) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = static_cast<int32_t>(p.x);
    data->point.y = static_cast<int32_t>(p.y);
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

void keyboardEventCb(lv_event_t* e) {
  PromptState* state = static_cast<PromptState*>(lv_event_get_user_data(e));
  if (state == nullptr) {
    return;
  }
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) {
    state->accepted = true;
    state->done = true;
  } else if (code == LV_EVENT_CANCEL) {
    state->accepted = false;
    state->done = true;
  }
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
  const size_t lines = 24;
  sBufSize = static_cast<size_t>(w) * lines * sizeof(lv_color16_t);
  sBuf1 = heap_caps_malloc(sBufSize, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  sBuf2 = heap_caps_malloc(sBufSize, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (sBuf1 == nullptr || sBuf2 == nullptr) {
    ESP_LOGE(kTag, "lvgl draw buffer alloc failed size=%u", static_cast<unsigned>(sBufSize));
    return false;
  }

  sDisplay = lv_display_create(w, h);
  if (sDisplay == nullptr) {
    ESP_LOGE(kTag, "lvgl display create failed");
    return false;
  }
  lv_display_set_buffers(sDisplay, sBuf1, sBuf2, sBufSize, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_color_format(sDisplay, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(sDisplay, flushCb);

  sTouchInput = lv_indev_create();
  if (sTouchInput == nullptr) {
    ESP_LOGE(kTag, "lvgl input create failed");
    return false;
  }
  lv_indev_set_type(sTouchInput, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(sTouchInput, touchReadCb);

  sLvglReady = true;
  ESP_LOGI(kTag, "lvgl ready");
  return true;
}

bool runPromptUi(const std::string& title, const std::string& subtitle, std::string& outPassword) {
  if (!ensureLvglReady()) {
    return false;
  }
  if (!touch_input::init()) {
    ESP_LOGE(kTag, "touch init failed for lvgl");
    return false;
  }

  PromptState state = {};
  lv_obj_t* screen = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x0A1222), 0);

  lv_obj_t* titleLbl = lv_label_create(screen);
  lv_label_set_text(titleLbl, title.empty() ? "WiFi Password" : title.c_str());
  lv_obj_set_style_text_color(titleLbl, lv_color_hex(0xE6EEF8), 0);
  lv_obj_align(titleLbl, LV_ALIGN_TOP_LEFT, 8, 6);

  lv_obj_t* subLbl = lv_label_create(screen);
  lv_label_set_long_mode(subLbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_label_set_text(subLbl, subtitle.c_str());
  lv_obj_set_style_text_color(subLbl, lv_color_hex(0x9AAFD2), 0);
  lv_obj_set_width(subLbl, AppConfig::kScreenWidth - 16);
  lv_obj_align(subLbl, LV_ALIGN_TOP_LEFT, 8, 28);

  lv_obj_t* ta = lv_textarea_create(screen);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_password_mode(ta, true);
  lv_textarea_set_password_show_time(ta, 0);
  lv_obj_set_size(ta, AppConfig::kScreenWidth - 16, 34);
  lv_obj_align(ta, LV_ALIGN_TOP_LEFT, 8, 48);

  lv_obj_t* kb = lv_keyboard_create(screen);
  lv_keyboard_set_textarea(kb, ta);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_set_size(kb, AppConfig::kScreenWidth, static_cast<int32_t>(AppConfig::kScreenHeight - 88));
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_event_cb(kb, keyboardEventCb, LV_EVENT_ALL, &state);

  lv_screen_load(screen);

  while (!state.done) {
    lv_tick_inc(kTickMs);
    (void)lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(kTickMs));
  }

  if (state.accepted) {
    outPassword = lv_textarea_get_text(ta);
  }

  lv_obj_del(screen);
  return state.accepted;
}

void promptTask(void* arg) {
  PromptTaskCtx* ctx = static_cast<PromptTaskCtx*>(arg);
  if (ctx != nullptr && ctx->outPassword != nullptr) {
    ctx->result = runPromptUi(ctx->title, ctx->subtitle, *ctx->outPassword);
  }
  if (ctx != nullptr && ctx->caller != nullptr) {
    xTaskNotifyGive(ctx->caller);
  }
  vTaskDelete(nullptr);
}

}  // namespace

namespace lvgl_password_prompt {

bool prompt(const std::string& title, const std::string& subtitle, std::string& outPassword) {
  PromptTaskCtx ctx = {};
  ctx.title = title;
  ctx.subtitle = subtitle;
  ctx.outPassword = &outPassword;
  ctx.caller = xTaskGetCurrentTaskHandle();

  TaskHandle_t task = nullptr;
  const BaseType_t created =
      xTaskCreatePinnedToCore(promptTask, "lvgl_prompt", kPromptTaskStackBytes, &ctx,
                              kPromptTaskPriority, &task, tskNO_AFFINITY);
  if (created != pdPASS) {
    ESP_LOGE(kTag, "create prompt task failed");
    return false;
  }

  (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  return ctx.result;
}

}  // namespace lvgl_password_prompt
