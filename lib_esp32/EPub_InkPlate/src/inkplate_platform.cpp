#include "inkplate_platform.hpp"

#if defined(BOARD_TYPE_PAPER_S3)

#include <cstdio>

#include "logging.hpp"

#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "esp_sleep.h"
#include "driver/sdmmc_types.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"

extern "C" {
  #include <epdiy.h>
}

// GT911 capacitive-touch INT line per the official M5Stack PaperS3
// PinMap (https://docs.m5stack.com/en/core/papers3 -> PinMap section).
// On ESP32-S3, GPIOs 0-21 are RTC-capable; GPIO 48 is NOT, so this
// pin can be used as a wake source for *light* sleep only. Deep sleep
// on this board exits exclusively via the hardware power button, which
// the PMS150 PMU handles as a full reset / cold boot.
static constexpr gpio_num_t GT911_INT_GPIO = GPIO_NUM_48;

InkPlatePlatform InkPlatePlatform::singleton;
InkPlatePlatform & inkplate_platform = InkPlatePlatform::get_singleton();

// Simple SD card state for Paper S3
static sdmmc_card_t * s_sd_card = nullptr;
static sdmmc_host_t   s_sd_host = SDSPI_HOST_DEFAULT();

bool InkPlatePlatform::setup(bool sd_card_init)
{
  LOG_I("Paper S3 InkPlatePlatform setup (sd_card_init=%d)", sd_card_init ? 1 : 0);

  if (sd_card_init && (s_sd_card == nullptr)) {
    esp_err_t ret;

    // Mount SD card at /sdcard using SPI host and the SD_CARD_PIN_NUM_* pins.
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 10,
      .allocation_unit_size = 16 * 1024
    };

    spi_bus_config_t bus_cfg = {
      .mosi_io_num = SD_CARD_PIN_NUM_MOSI,
      .miso_io_num = SD_CARD_PIN_NUM_MISO,
      .sclk_io_num = SD_CARD_PIN_NUM_CLK,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = 0,
      .flags = 0,
      .intr_flags = 0
    };

    ret = spi_bus_initialize(static_cast<spi_host_device_t>(s_sd_host.slot), &bus_cfg, SPI_DMA_CH_AUTO);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
      LOG_E("Paper S3: Failed to initialize SD SPI bus (%s)", esp_err_to_name(ret));
      return false;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CARD_PIN_NUM_CS;
    slot_config.host_id = static_cast<spi_host_device_t>(s_sd_host.slot);

    ret = esp_vfs_fat_sdspi_mount("/sdcard", &s_sd_host, &slot_config, &mount_config, &s_sd_card);
    if (ret != ESP_OK) {
      LOG_E("Paper S3: Failed to mount SD card at /sdcard (%s)", esp_err_to_name(ret));
      return false;
    }

    sdmmc_card_print_info(stdout, s_sd_card);
  }

  return true;
}

bool InkPlatePlatform::light_sleep(uint32_t minutes_to_sleep, gpio_num_t /*gpio_num*/, int /*level*/)
{
  // Configure the GT911 INT line as a level-triggered wake source.
  // The GT911 holds the INT line low while a touch is in progress and
  // pulses it briefly on touch events. Any low level wakes the chip.
  gpio_set_direction(GT911_INT_GPIO, GPIO_MODE_INPUT);
  gpio_pullup_en(GT911_INT_GPIO);
  gpio_pulldown_dis(GT911_INT_GPIO);
  esp_sleep_enable_gpio_wakeup();
  gpio_wakeup_enable(GT911_INT_GPIO, GPIO_INTR_LOW_LEVEL);

  // Optional timer wake. Disabled when minutes_to_sleep == 0 so the
  // device sleeps until touched. Useful for the auto-idle path that
  // wants periodic wake to refresh the battery indicator.
  if (minutes_to_sleep > 0) {
    uint64_t us = (uint64_t)minutes_to_sleep * 60ull * 1000000ull;
    esp_sleep_enable_timer_wakeup(us);
  }

  LOG_I("Paper S3 entering light sleep (touch wake on GPIO %d, timer=%u min)",
        (int)GT911_INT_GPIO, (unsigned)minutes_to_sleep);

  esp_light_sleep_start();

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  gpio_wakeup_disable(GT911_INT_GPIO);

  LOG_I("Paper S3 woke from light sleep (cause=%d)", (int)cause);
  // Match the existing caller semantics in event_mgr / touch_event_mgr:
  //   true  → timer expired (caller escalates to deep_sleep)
  //   false → GPIO touch wake (caller resumes normal operation)
  return cause == ESP_SLEEP_WAKEUP_TIMER;
}

void InkPlatePlatform::deep_sleep(gpio_num_t /*gpio_num*/, int /*level*/)
{
  LOG_I("Paper S3 entering deep sleep (wake via hardware power button)");

  // Power down the e-paper rail. The image already on the panel stays
  // visible without power (e-ink physics) — whatever the caller drew
  // before invoking deep_sleep remains the visible state until wake.
  epd_poweroff();

  // No software wake source — GT911 INT lives on GPIO 48 which is NOT
  // RTC-capable on ESP32-S3, so it cannot fire ext0/ext1 wake. The
  // only way out of deep sleep on this board is the hardware power
  // button, which the PMS150 PMU drives as a cold reset.
  //
  // The BM8563 RTC chip's INT line is wired through PMS150, not to
  // any ESP32 GPIO, so RTC alarms also wake via the PMU reset path
  // (effectively a cold boot at the alarm time). RTC alarm wake is
  // not configured here — left as a future enhancement.

  esp_deep_sleep_start();
  // Never returns.
}

#endif // BOARD_TYPE_PAPER_S3
