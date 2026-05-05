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
#include "tinyusb_msc.h"

namespace
{
  static constexpr char const * TAG = "UsbMsc";

  // True between a successful start() and exit_via_restart(). Never
  // cleared in-process: the only exit path is esp_restart(), which
  // re-initializes everything from a fresh boot. Keeping the field
  // here (rather than a local in start()) lets is_active() report
  // the live state to UI callers without round-tripping through
  // TinyUSB.
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

  // v2 esp_tinyusb manages the FATFS-vs-USB ownership of the card
  // internally — we register the storage with the live card handle
  // and TinyUSB swaps the mount point to USB when we ask. Do NOT
  // unmount FATFS ourselves: esp_vfs_fat_sdcard_unmount() free()s
  // the sdmmc_card_t allocation, and TinyUSB would then issue
  // block I/O against a dangling pointer for the entire host
  // session.
  tinyusb_msc_sdmmc_config_t msc_cfg = {};
  msc_cfg.card = card;
  esp_err_t ret = tinyusb_msc_new_storage_sdmmc(&msc_cfg);
  if (ret != ESP_OK) {
    LOG_E("tinyusb_msc_new_storage_sdmmc failed (%s)", esp_err_to_name(ret));
    return false;
  }

  // Switch ownership of the storage from the application's FATFS
  // mount to the USB host. v2 esp_tinyusb internally handles the
  // FATFS unmount, the card-handle preservation, and the host-side
  // notification — replacing the unmount_sd_fat() call we used in
  // the v1 shim.
  ret = tinyusb_msc_storage_mount_to_usb();
  if (ret != ESP_OK) {
    LOG_E("tinyusb_msc_storage_mount_to_usb failed (%s)", esp_err_to_name(ret));
    tinyusb_msc_delete_storage();
    return false;
  }

  // Install the TinyUSB device driver. With CDC disabled in
  // sdkconfig and only MSC enabled, the resulting USB device is a
  // single-interface MSC drive on the host's enumeration tree.
  // Default descriptors come from the Kconfig values
  // (CONFIG_TINYUSB_DESC_*) set in sdkconfig.paper_s3.
  tinyusb_config_t tusb_cfg = {};
  ret = tinyusb_driver_install(&tusb_cfg);
  if (ret != ESP_OK) {
    LOG_E("tinyusb_driver_install failed (%s)", esp_err_to_name(ret));
    tinyusb_msc_delete_storage();
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
