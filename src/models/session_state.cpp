// Copyright (c) 2026 Patrick Smith
//
// MIT License. Look at file licenses.txt for details.

#include "models/session_state.hpp"

#if EPUB_INKPLATE_BUILD

#include "logging.hpp"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_err.h"

namespace
{
  static constexpr char const * TAG       = "SessionState";
  static constexpr char const * NAMESPACE = "session";
  static constexpr char const * KEY       = "deep_sleep";

  // Process-local cache. Set once by init_at_boot(); read by
  // is_warm_wake() throughout the rest of the run.
  bool s_warm_wake   = false;
  bool s_initialized = false;
}

void SessionState::init_at_boot()
{
  if (s_initialized) {
    LOG_W("init_at_boot called twice; ignoring");
    return;
  }
  s_initialized = true;
  s_warm_wake   = false;

  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    LOG_W("nvs_open(%s) failed (%s); assuming cold boot", NAMESPACE, esp_err_to_name(err));
    return;
  }

  uint8_t marker = 0;
  err = nvs_get_u8(h, KEY, &marker);
  if (err == ESP_OK) {
    s_warm_wake = (marker != 0);
  }
  // ESP_ERR_NVS_NOT_FOUND on first boot is normal; warm_wake stays false.

  // Always clear the persisted marker now that we've consumed it.
  // If anything later in boot crashes, the next boot will see the
  // marker absent and run the safe cold-boot path.
  //
  // If the clear write fails (NVS full / corrupted), the persisted
  // marker stays 1 and the *next* boot will be classified as warm
  // again — at worst a single extra "skipped epd_fullclear" cycle.
  // The display logic is idempotent under that case (the next real
  // render is still a forced GC16) so the worst observable effect
  // is a slightly stale panel image instead of an all-white one.
  if (s_warm_wake) {
    esp_err_t set_err = nvs_set_u8(h, KEY, 0);
    if (set_err == ESP_OK) {
      nvs_commit(h);
    } else {
      LOG_W("Failed to clear deep-sleep marker (%s); next boot may also classify as warm",
            esp_err_to_name(set_err));
    }
  }

  nvs_close(h);

  LOG_I("Boot type: %s", s_warm_wake ? "warm wake from deep sleep" : "cold boot");
}

bool SessionState::is_warm_wake()
{
  return s_initialized && s_warm_wake;
}

void SessionState::clear_warm_wake()
{
  if (s_warm_wake) {
    LOG_D("Warm-wake budget consumed; subsequent transitions get normal UX");
  }
  s_warm_wake = false;
}

void SessionState::mark_entering_deep_sleep()
{
  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    LOG_W("nvs_open(%s) failed (%s); next boot will be cold", NAMESPACE, esp_err_to_name(err));
    return;
  }
  if (nvs_set_u8(h, KEY, 1) == ESP_OK) {
    nvs_commit(h);
    LOG_I("Persisted deep-sleep marker; next boot will take warm-wake path");
  }
  nvs_close(h);
}

#endif // EPUB_INKPLATE_BUILD
