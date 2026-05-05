// Minimal InkPlatePlatform shim for ESP32 builds.
// For Inkplate boards, this header defers to the original
// Inkplate platform implementation from the ESP-IDF-Inkplate
// library. For BOARD_TYPE_PAPER_S3, it provides a stub
// implementation that we will extend to use epdiy.

#pragma once

#include "global.hpp"
#include "non_copyable.hpp"

#if defined(BOARD_TYPE_PAPER_S3)

#include "driver/gpio.h"
#include "driver/sdmmc_types.h"

class InkPlatePlatform : NonCopyable
{
private:
  static constexpr char const * TAG = "InkPlatePlatform";
  static InkPlatePlatform singleton;
  InkPlatePlatform() = default;

public:
  static inline InkPlatePlatform & get_singleton() noexcept { return singleton; }

  // For now, setup/light_sleep/deep_sleep are minimal stubs.
  bool setup(bool sd_card_init = false);
  bool light_sleep(uint32_t minutes_to_sleep, gpio_num_t gpio_num = (gpio_num_t)0, int level = 1);
  void deep_sleep(gpio_num_t gpio_num = (gpio_num_t)0, int level = 1);

  /// Hand the live SD-card handle to whoever needs raw block access
  /// (USB Mass Storage). Returns nullptr if the card hasn't been
  /// mounted yet (setup() with sd_card_init=true must run first).
  /// Callers must NOT free the handle — ownership stays with the
  /// platform.
  sdmmc_card_t * get_sd_card();

  /// Tear down the FATFS layer over the SD card so a different
  /// owner (e.g. TinyUSB MSC) can take exclusive block-level
  /// access. The card itself stays initialized and the handle
  /// returned by get_sd_card() remains valid; only the VFS mount
  /// at /sdcard is removed. Returns true on success.
  ///
  /// There is intentionally no remount-fat() counterpart: the
  /// USB-Drive Mode flow exits via esp_restart() so the next mount
  /// happens in a fresh boot with no risk of stale FATFS state.
  bool unmount_sd_fat();
};

extern InkPlatePlatform & inkplate_platform;

#else

// Non-Paper S3 builds should use the original Inkplate
// platform implementation provided by ESP-IDF-Inkplate.
#include_next "inkplate_platform.hpp"

#endif
