// Copyright (c) 2026 Patrick Smith
//
// MIT License. Look at file licenses.txt for details.
//
// USB Mass Storage Class wrapper for M5Stack PaperS3.
//
// Drives the on-chip USB OTG peripheral as a USB MSC device backed by
// the on-board microSD card so a host computer sees the SD as a
// mounted drive. Used by the "USB Drive Mode" feature surfaced from
// the Settings menu.
//
// Lifecycle:
//   1. The reader's normal book/menu UI is torn down by the caller
//      (close epub, clear fonts, mark stay_on, etc.) — same setup
//      pattern the existing WiFi mode uses.
//   2. UsbMsc::start() unmounts the FATFS overlay so TinyUSB can take
//      exclusive block-level access, then installs the TinyUSB driver
//      configured for MSC class. Returns true on success.
//   3. The host computer enumerates a removable disk and the user
//      drag-drops files normally. The ESP32 application is otherwise
//      idle — TinyUSB handles every read/write transparently.
//   4. The user explicitly exits via a key/touch event, the caller
//      shows a "Restarting…" message and invokes esp_restart(). We
//      do NOT attempt to re-mount FATFS in-place; the restart gives
//      us a guaranteed-clean boot that picks up any new files via
//      the normal books_dir refresh.
//
// Why no in-place exit:
//   TinyUSB's MSC layer hooks deep into ESP-IDF's USB stack and
//   removing it cleanly while the SD card is potentially in mid-
//   write from the host is fragile. The existing WiFi mode does
//   the same esp_restart() trick on exit (see option_controller.cpp);
//   we just follow the established pattern.

#pragma once
#include "global.hpp"

#if EPUB_INKPLATE_BUILD && defined(BOARD_TYPE_PAPER_S3)

namespace UsbMsc {

  // Identifies which step of start() failed, surfaced through the
  // last_error_*() accessors so the option_controller can print a
  // specific reason on the alert dialog instead of the generic
  // "Could not initialize" message. Useful for field diagnosis when
  // a serial log isn't available.
  enum class StartError {
    NONE,
    NO_SD_CARD,                ///< inkplate_platform.get_sd_card() returned nullptr
    NEW_STORAGE_SDMMC_FAILED,  ///< tinyusb_msc_new_storage_sdmmc returned non-OK
    DRIVER_INSTALL_FAILED,     ///< tinyusb_driver_install returned non-OK
  };

  /// Initialize the USB stack as a MSC device backed by the on-board
  /// microSD card. The reader's FATFS mount is torn down inside this
  /// call so the host has exclusive block-level access. Returns true
  /// when the device successfully starts presenting on the USB bus.
  /// On any failure path the SD card is left unmounted; caller should
  /// treat the device state as needing a restart.
  bool start();

  /// After a failed start(), returns the step at which it failed.
  /// NONE on success.
  StartError last_error();
  /// After a failed start(), returns the esp_err_t from the failing
  /// API call (or ESP_OK if the failure was NO_SD_CARD which has no
  /// ESP error code).
  int last_error_code();

  /// True between a successful start() and an exit_via_restart(). Used
  /// by callers that want to render a different UI state while in MSC
  /// mode.
  bool is_active();

  /// Display-side helper: caller invokes this from its "exit MSC"
  /// branch after rendering a "Restarting…" message. Handles any
  /// platform-specific shutdown bookkeeping then calls esp_restart()
  /// — never returns.
  [[noreturn]] void exit_via_restart();

}

#endif // EPUB_INKPLATE_BUILD && BOARD_TYPE_PAPER_S3
