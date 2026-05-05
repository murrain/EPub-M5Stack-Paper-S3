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
  /// platform. Callers must NOT call esp_vfs_fat_sdcard_unmount on
  /// it either; that routine frees the underlying allocation. If a
  /// caller needs exclusive block access (e.g. USB MSC) it should
  /// use the storage layer's own ownership-transfer API instead.
  sdmmc_card_t * get_sd_card();
};

extern InkPlatePlatform & inkplate_platform;

#else

// Non-Paper S3 builds should use the original Inkplate
// platform implementation provided by ESP-IDF-Inkplate.
#include_next "inkplate_platform.hpp"

#endif
