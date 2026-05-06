// Paper S3 EventMgr stub implementation
// Provides a minimal, no-input EventMgr so the EPUB app can run
// on BOARD_TYPE_PAPER_S3 without Inkplate-specific key handling.

#include "global.hpp"

#if defined(BOARD_TYPE_PAPER_S3)

#include "controllers/event_mgr.hpp"
#include "controllers/app_controller.hpp"
#include "screen.hpp"

#if EPUB_INKPLATE_BUILD
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"
  #include "freertos/queue.h"
  #include "driver/i2c.h"
  #include "esp_log.h"
#endif

EventMgr event_mgr;

#if EPUB_INKPLATE_BUILD

static constexpr char const * TAG = "EventMgrPaperS3";

static const gpio_num_t PAPERS3_GT911_SDA_GPIO = GPIO_NUM_41;
static const gpio_num_t PAPERS3_GT911_SCL_GPIO = GPIO_NUM_42;
static const i2c_port_t PAPERS3_GT911_I2C_PORT = I2C_NUM_0;

static uint8_t gt911_addr = 0x14;
static bool    gt911_ok   = false;

static QueueHandle_t input_event_queue = nullptr;

static esp_err_t gt911_write_reg(uint8_t addr, uint16_t reg, const uint8_t * data, size_t len)
{
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  if (cmd == nullptr) return ESP_FAIL;

  uint8_t reg_hi = reg >> 8;
  uint8_t reg_lo = reg & 0xFF;

  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write_byte(cmd, reg_hi, true);
  i2c_master_write_byte(cmd, reg_lo, true);
  if ((data != nullptr) && (len != 0)) {
    i2c_master_write(cmd, (uint8_t *)data, len, true);
  }
  i2c_master_stop(cmd);

  esp_err_t ret = i2c_master_cmd_begin(PAPERS3_GT911_I2C_PORT, cmd, pdMS_TO_TICKS(100));
  i2c_cmd_link_delete(cmd);
  return ret;
}

static esp_err_t gt911_read_reg(uint8_t addr, uint16_t reg, uint8_t * data, size_t len)
{
  if ((data == nullptr) || (len == 0)) return ESP_ERR_INVALID_ARG;

  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  if (cmd == nullptr) return ESP_FAIL;

  uint8_t reg_hi = reg >> 8;
  uint8_t reg_lo = reg & 0xFF;

  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write_byte(cmd, reg_hi, true);
  i2c_master_write_byte(cmd, reg_lo, true);

  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);

  if (len > 1) {
    i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
  }
  i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
  i2c_master_stop(cmd);

  esp_err_t ret = i2c_master_cmd_begin(PAPERS3_GT911_I2C_PORT, cmd, pdMS_TO_TICKS(100));
  i2c_cmd_link_delete(cmd);
  return ret;
}

// Tri-state result of polling the GT911 controller. Distinguishing NO_DATA
// from UP is critical: the GT911 buffer-ready bit drops between coordinate
// updates during an in-progress swipe, and treating those polls as a release
// would prematurely classify the gesture as a tap.
enum class TouchSample : uint8_t { DOWN, NO_DATA, UP };

static TouchSample gt911_sample(uint16_t * x, uint16_t * y)
{
  if (!gt911_ok || (x == nullptr) || (y == nullptr)) return TouchSample::UP;

  uint8_t status = 0;
  // A transient I2C glitch on either read should be treated as NO_DATA, not
  // UP, so an in-progress gesture is preserved across the hiccup rather than
  // being silently terminated.
  if (gt911_read_reg(gt911_addr, 0x814E, &status, 1) != ESP_OK) {
    return TouchSample::NO_DATA;
  }

  // Buffer-not-ready: no fresh sample this poll. Do NOT ack — caller treats
  // the previous touch state as still in effect.
  if ((status & 0x80) == 0) return TouchSample::NO_DATA;

  uint8_t points = status & 0x0F;
  if (points == 0) {
    uint8_t zero = 0;
    gt911_write_reg(gt911_addr, 0x814E, &zero, 1);
    return TouchSample::UP;
  }

  uint8_t data[4] = { 0 };
  if (gt911_read_reg(gt911_addr, 0x8150, data, sizeof(data)) != ESP_OK) {
    return TouchSample::NO_DATA;
  }

  *x = (uint16_t)((data[1] << 8) | data[0]);
  *y = (uint16_t)((data[3] << 8) | data[2]);

  uint8_t zero = 0;
  gt911_write_reg(gt911_addr, 0x814E, &zero, 1);

  return TouchSample::DOWN;
}

static void touch_task(void * param)
{
  (void)param;

  // Simple gesture state machine inspired by Inkplate-6PLUS touch handling.
  // We interpret GT911 samples as a single-finger stream and classify each
  // interaction as a TAP, horizontal SWIPE_LEFT / SWIPE_RIGHT, or HOLD /
  // RELEASE. Coordinates are reported in the logical Screen space.

  constexpr uint16_t swipe_distance_threshold  =  60;  // pixels — was 100
  constexpr uint16_t swipe_velocity_min_dx     =  40;  // pixels for fast-flick path
  constexpr uint32_t swipe_velocity_max_dt_ms  = 200;  // ms — fast flick must lift within this window
  constexpr uint16_t longpress_move_threshold  =  30;  // max motion during hold
  constexpr uint32_t longpress_ms              = 600;  // press duration
  constexpr uint32_t poll_interval_ms          =  20;  // was 50

  bool       touch_active   = false;
  bool       hold_sent      = false;
  uint16_t   start_x        = 0;
  uint16_t   start_y        = 0;
  uint16_t   current_x      = 0;
  uint16_t   current_y      = 0;
  TickType_t start_tick     = 0;
  int        max_signed_dx  = 0;  // peak signed X displacement during touch
  int        max_signed_dy  = 0;  // peak signed Y displacement during touch
  int        max_abs_dy     = 0;  // peak |Y| displacement during touch

  while (true) {
    uint16_t x = 0;
    uint16_t y = 0;

    TouchSample sample = gt911_sample(&x, &y);

    // NO_DATA means the GT911 had no fresh buffer this poll. The finger is
    // still presumed to be where it was; skip processing entirely so a brief
    // gap in the controller's stream cannot be misread as a release.
    if (sample == TouchSample::NO_DATA) {
      vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
      continue;
    }

    bool has_touch = (sample == TouchSample::DOWN);

    if (has_touch) {
      if (!touch_active) {
        // First contact
        touch_active   = true;
        hold_sent      = false;
        start_tick     = xTaskGetTickCount();
        start_x        = current_x = x;
        start_y        = current_y = y;
        max_signed_dx  = 0;
        max_signed_dy  = 0;
        max_abs_dy     = 0;
      }
      else {
        // Update current finger position while it moves.
        current_x = x;
        current_y = y;
      }

      // Track peak displacement seen during this touch. The classifier on
      // release uses these instead of endpoint deltas so a swipe that
      // partly returns or whose final sample missed the peak still
      // registers correctly. Both signed peaks (max_signed_dx, max_
      // signed_dy) preserve direction so SWIPE_LEFT vs RIGHT and
      // SWIPE_UP vs DOWN can be distinguished on release.
      {
        int cur_dx = (int)current_x - (int)start_x;
        int cur_dy = (int)current_y - (int)start_y;

        int abs_cur_dx     = cur_dx >= 0 ? cur_dx : -cur_dx;
        int abs_cur_dy     = cur_dy >= 0 ? cur_dy : -cur_dy;
        int abs_max_dx_now = max_signed_dx >= 0 ? max_signed_dx : -max_signed_dx;
        int abs_max_dy_now = max_signed_dy >= 0 ? max_signed_dy : -max_signed_dy;
        if (abs_cur_dx > abs_max_dx_now) max_signed_dx = cur_dx;
        if (abs_cur_dy > abs_max_dy_now) max_signed_dy = cur_dy;
        if (abs_cur_dy > max_abs_dy)     max_abs_dy    = abs_cur_dy;
      }

      // Detect a long press while the finger is still down.
      if (touch_active && !hold_sent) {
        TickType_t now   = xTaskGetTickCount();
        uint32_t   dt_ms = (now - start_tick) * portTICK_PERIOD_MS;

        int dx = (int)current_x - (int)start_x;
        int dy = (int)current_y - (int)start_y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;

        if ((dt_ms >= longpress_ms) &&
            (dx <= (int)longpress_move_threshold) &&
            (dy <= (int)longpress_move_threshold)) {

          EventMgr::Event ev;
          ev.kind = EventMgr::EventKind::HOLD;
          ev.x    = start_x;
          ev.y    = start_y;
          ev.dist = 0;

          if (input_event_queue != nullptr) {
            xQueueSend(input_event_queue, &ev, 0);
          }

          hold_sent = true;
        }
      }
    }
    else {
      if (touch_active) {
        // Touch has just ended – classify the gesture.
        touch_active = false;

        EventMgr::Event ev;
        ev.x    = start_x;
        ev.y    = start_y;
        ev.dist = 0;
        ev.kind = EventMgr::EventKind::NONE;

        TickType_t end_tick = xTaskGetTickCount();
        uint32_t   dt_ms    = (end_tick - start_tick) * portTICK_PERIOD_MS;

        int abs_max_dx = max_signed_dx >= 0 ? max_signed_dx : -max_signed_dx;

        int abs_max_dy_signed = max_signed_dy >= 0 ? max_signed_dy : -max_signed_dy;

        if (hold_sent) {
          ev.kind = EventMgr::EventKind::RELEASE;
        }
        else if (abs_max_dx > abs_max_dy_signed) {
          // Predominantly horizontal motion — candidate for left/right swipe.
          bool meets_distance = abs_max_dx > (int)swipe_distance_threshold;
          bool meets_velocity = (abs_max_dx > (int)swipe_velocity_min_dx)
                             && (dt_ms      < swipe_velocity_max_dt_ms);

          if (meets_distance || meets_velocity) {
            ev.kind = (max_signed_dx > 0) ? EventMgr::EventKind::SWIPE_RIGHT
                                          : EventMgr::EventKind::SWIPE_LEFT;
          } else {
            ev.kind = EventMgr::EventKind::TAP;
          }
        }
        else if (abs_max_dy_signed > abs_max_dx) {
          // Predominantly vertical motion — candidate for down/up swipe.
          // Reuses the same distance + velocity thresholds as the
          // horizontal classifier; the gesture grammar should feel
          // consistent across axes.
          bool meets_distance = abs_max_dy_signed > (int)swipe_distance_threshold;
          bool meets_velocity = (abs_max_dy_signed > (int)swipe_velocity_min_dx)
                             && (dt_ms             < swipe_velocity_max_dt_ms);

          if (meets_distance || meets_velocity) {
            ev.kind = (max_signed_dy > 0) ? EventMgr::EventKind::SWIPE_DOWN
                                          : EventMgr::EventKind::SWIPE_UP;
          } else {
            ev.kind = EventMgr::EventKind::TAP;
          }
        }
        else {
          ev.kind = EventMgr::EventKind::TAP;
        }

        if ((ev.kind != EventMgr::EventKind::NONE) && (input_event_queue != nullptr)) {
          xQueueSend(input_event_queue, &ev, 0);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
  }
}

#endif // EPUB_INKPLATE_BUILD

bool EventMgr::setup()
{
#if EPUB_INKPLATE_BUILD
  if (input_event_queue == nullptr) {
    input_event_queue = xQueueCreate(10, sizeof(Event));
  }

  i2c_config_t conf = {};
  conf.mode = I2C_MODE_MASTER;
  conf.sda_io_num = PAPERS3_GT911_SDA_GPIO;
  conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
  conf.scl_io_num = PAPERS3_GT911_SCL_GPIO;
  conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
  conf.master.clk_speed = 400000;

  esp_err_t err = i2c_param_config(PAPERS3_GT911_I2C_PORT, &conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2c_param_config failed: %d", (int)err);
  }
  else {
    err = i2c_driver_install(PAPERS3_GT911_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
      ESP_LOGE(TAG, "i2c_driver_install failed: %d", (int)err);
    }
    else {
      uint8_t buf = 0;
      if (gt911_read_reg(0x14, 0x8140, &buf, 1) == ESP_OK) {
        gt911_addr = 0x14;
        gt911_ok   = true;
        ESP_LOGI(TAG, "GT911 detected at 0x14");
      }
      else if (gt911_read_reg(0x5D, 0x8140, &buf, 1) == ESP_OK) {
        gt911_addr = 0x5D;
        gt911_ok   = true;
        ESP_LOGI(TAG, "GT911 detected at 0x5D");
      }
      else {
        ESP_LOGE(TAG, "GT911 not found on I2C bus");
      }
    }
  }

  TaskHandle_t handle = nullptr;
  xTaskCreatePinnedToCore(touch_task, "papers3_touch", 4096, nullptr, 5, &handle, 1);
#endif

  return true;
}

void EventMgr::loop()
{
#if EPUB_INKPLATE_BUILD
  while (true) {
    const Event & event = get_event();

    if (event.kind != EventKind::NONE) {
      app_controller.input_event(event);
      return;
    }
  }
#else
  while (true) { }
#endif
}

const EventMgr::Event & EventMgr::get_event()
{
  static Event event{ EventKind::NONE };

#if EPUB_INKPLATE_BUILD
  if (input_event_queue == nullptr) {
    event.kind = EventKind::NONE;
    vTaskDelay(pdMS_TO_TICKS(1000));
    return event;
  }

  if (!xQueueReceive(input_event_queue, &event, portMAX_DELAY)) {
    event.kind = EventKind::NONE;
  }
#endif

  return event;
}

void EventMgr::set_orientation(Screen::Orientation)
{
}

#endif // BOARD_TYPE_PAPER_S3
