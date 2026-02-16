#include "inkplate_platform.hpp"

#if defined(BOARD_TYPE_PAPER_S3)

#include <cstdio>

#include "logging.hpp"

#include "esp_err.h"
#include "esp_sleep.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_types.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"

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

/// Enter light sleep with timer + boot button (GPIO 0) wake sources.
/// @param minutes_to_sleep Duration before timer wake
/// @param gpio_num Unused — wake GPIO is hardcoded to GPIO_NUM_0 (boot button)
/// @param level   Unused — wake level is hardcoded to 0 (active low)
/// @return true if woken by timer (caller should escalate to deep sleep),
///         false if woken by button press (resume normal operation)
bool InkPlatePlatform::light_sleep(uint32_t minutes_to_sleep, gpio_num_t gpio_num, int level)
{
  (void)gpio_num;
  (void)level;

  LOG_I("Paper S3: entering light sleep for %u minutes", minutes_to_sleep);

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_timer_wakeup((uint64_t)minutes_to_sleep * 60ULL * 1000000ULL);

  // Boot button (GPIO 0, active low) as manual wake source
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);

  esp_err_t ret = esp_light_sleep_start();
  if (ret != ESP_OK) {
    LOG_E("Paper S3: esp_light_sleep_start failed (%s)", esp_err_to_name(ret));
    return false;
  }

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  LOG_I("Paper S3: woke from light sleep (cause=%d)", (int)cause);

  // Timer expiry means the user didn't interact — proceed to deep sleep
  return (cause == ESP_SLEEP_WAKEUP_TIMER);
}

/// Enter deep sleep (full power-down). Only the boot button (GPIO 0) wakes.
/// This function does NOT return — the device reboots on wake.
/// @param gpio_num Unused — hardcoded to GPIO_NUM_0
/// @param level   Unused — hardcoded to 0 (active low)
void InkPlatePlatform::deep_sleep(gpio_num_t gpio_num, int level)
{
  (void)gpio_num;
  (void)level;

  LOG_I("Paper S3: entering deep sleep");

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

  // Boot button (GPIO 0, active low) as wake source
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);

  esp_deep_sleep_start();
  // Does not return
}

#endif // BOARD_TYPE_PAPER_S3
