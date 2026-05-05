// Copyright (c) 2026 Patrick Smith
//
// MIT License. Look at file licenses.txt for details.

#include "usb_msc_paper_s3.hpp"

#if EPUB_INKPLATE_BUILD && defined(BOARD_TYPE_PAPER_S3)

#include "logging.hpp"
#include "inkplate_platform.hpp"

#include "esp_err.h"
#include "esp_system.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"

namespace
{
  static constexpr char const * TAG = "UsbMsc";

  bool s_active = false;
}

bool UsbMsc::start()
{
  if (s_active) {
    LOG_W("UsbMsc::start called twice; ignoring");
    return true;
  }

  sdmmc_card_t * card = inkplate_platform.get_sd_card();
  if (card == nullptr) {
    LOG_E("UsbMsc::start: no SD card handle (call inkplate_platform.setup(true) first)");
    return false;
  }

  // Hand the SD card over to TinyUSB MSC. The wrapper takes a copy
  // of what it needs; it does NOT take ownership of the handle, so
  // we don't null out our own reference here.
  tinyusb_msc_sdmmc_config_t msc_cfg = {};
  msc_cfg.card = card;
  // Leave callbacks (mount_changed_cb, etc.) as nullptr — we don't
  // need to react to host mount/unmount events for v1. The pattern
  // is well-supported by the esp_tinyusb component.
  esp_err_t ret = tinyusb_msc_storage_init_sdmmc(&msc_cfg);
  if (ret != ESP_OK) {
    LOG_E("tinyusb_msc_storage_init_sdmmc failed (%s)", esp_err_to_name(ret));
    return false;
  }

  // Tear down the FATFS overlay so the host has exclusive block
  // access. Done AFTER the MSC storage init succeeds so we don't
  // leave the card unmounted on a pure-init failure.
  if (!inkplate_platform.unmount_sd_fat()) {
    LOG_E("Failed to unmount FATFS for USB-Drive Mode; aborting");
    tinyusb_msc_storage_deinit();
    return false;
  }

  // Install the TinyUSB device driver. With CDC disabled in
  // sdkconfig and only MSC enabled, the resulting USB device is a
  // single-interface MSC drive on the host's enumeration tree.
  tinyusb_config_t tusb_cfg = {};
  // Default descriptors come from the Kconfig values
  // (CONFIG_TINYUSB_DESC_*) we set in sdkconfig.paper_s3.
  ret = tinyusb_driver_install(&tusb_cfg);
  if (ret != ESP_OK) {
    LOG_E("tinyusb_driver_install failed (%s)", esp_err_to_name(ret));
    tinyusb_msc_storage_deinit();
    return false;
  }

  s_active = true;
  LOG_I("USB Drive Mode active — host should see a removable disk");
  return true;
}

bool UsbMsc::is_active()
{
  return s_active;
}

[[noreturn]] void UsbMsc::exit_via_restart()
{
  LOG_I("USB Drive Mode exiting via esp_restart()");
  // We deliberately do NOT call tinyusb_driver_uninstall() or
  // tinyusb_msc_storage_deinit() here. The host may be mid-write
  // (filesystem journal flush, lazy metadata commit, etc.); ripping
  // the device out from under the OS is exactly what we want to
  // avoid. The reset gives the host a clean disconnect event and
  // the device boots fresh with the FATFS re-mounted from scratch
  // by the normal cold-boot path.
  esp_restart();
  // Unreachable but the [[noreturn]] attribute makes the compiler
  // enforce that any caller stops there too. esp_restart never
  // returns; if it ever did, fall through to a hang.
  for (;;) {}
}

#endif // EPUB_INKPLATE_BUILD && BOARD_TYPE_PAPER_S3
