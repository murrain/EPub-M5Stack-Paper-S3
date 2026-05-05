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
  //
  // CONTRACT: this flag's "set once, never cleared" lifecycle is
  // load-bearing on the esp_restart-based exit. If the design ever
  // changes to support in-process MSC exit (tinyusb_msc_set_storage_
  // mount_point back to MOUNT_APP, then tinyusb_driver_uninstall),
  // s_active MUST be cleared on that path or is_active() will lie
  // to subsequent callers.
  bool s_active = false;

  // Handle returned by tinyusb_msc_new_storage_sdmmc. Needed for the
  // failure-path cleanup calls (tinyusb_msc_delete_storage takes the
  // handle) and would also be needed for any future mount-point
  // toggle (tinyusb_msc_set_storage_mount_point).
  tinyusb_msc_storage_handle_t s_msc_handle = nullptr;
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
  // and the storage's `mount_point` field tells the driver to start
  // in USB-host mode. Do NOT call esp_vfs_fat_sdcard_unmount on the
  // card pointer ourselves: that routine free()s the sdmmc_card_t
  // allocation, and TinyUSB would then issue block I/O against a
  // dangling pointer for the entire host session.
  tinyusb_msc_storage_config_t msc_cfg = {};
  msc_cfg.medium.card = card;
  msc_cfg.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;

  esp_err_t ret = tinyusb_msc_new_storage_sdmmc(&msc_cfg, &s_msc_handle);
  if (ret != ESP_OK) {
    LOG_E("tinyusb_msc_new_storage_sdmmc failed (%s)", esp_err_to_name(ret));
    return false;
  }

  // Install the MSC class driver: registers LUNs and the SCSI
  // emulation that translates host bulk transfers into block I/O
  // against the storage handle above. Defaults zero-init to the
  // recommended values (no auto-mount-off, no callbacks).
  tinyusb_msc_driver_config_t msc_drv_cfg = {};
  ret = tinyusb_msc_install_driver(&msc_drv_cfg);
  if (ret != ESP_OK) {
    LOG_E("tinyusb_msc_install_driver failed (%s)", esp_err_to_name(ret));
    tinyusb_msc_delete_storage(s_msc_handle);
    s_msc_handle = nullptr;
    return false;
  }

  // Install the TinyUSB device driver — this is what actually
  // brings the USB peripheral up and starts host enumeration.
  // Default descriptors come from the Kconfig values
  // (CONFIG_TINYUSB_DESC_*) set in sdkconfig.paper_s3.
  //
  // Note on partial-init rollback: on driver_install failure we
  // tear down the MSC storage but do NOT call
  // tinyusb_msc_set_storage_mount_point(MOUNT_APP) to restore
  // FATFS to the application — the v2 storage was created in
  // MOUNT_USB mode so FATFS was never bound. /sdcard is therefore
  // unavailable for the rest of this boot if we hit this path,
  // which is acceptable because the alert message_viewer instructs
  // the user to power-cycle on failure (see option_controller).
  tinyusb_config_t tusb_cfg = {};
  ret = tinyusb_driver_install(&tusb_cfg);
  if (ret != ESP_OK) {
    LOG_E("tinyusb_driver_install failed (%s)", esp_err_to_name(ret));
    tinyusb_msc_delete_storage(s_msc_handle);
    s_msc_handle = nullptr;
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
