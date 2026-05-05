// Copyright (c) 2026 Patrick Smith
//
// MIT License. Look at file licenses.txt for details.
//
// Session-state marker: distinguishes a "warm wake" (boot following a
// clean deep_sleep) from a cold boot (first power-on, hardware error,
// or unclean reset). Used to gate fast-path optimizations during boot.
//
// The mechanism is a single uint8 in a dedicated NVS namespace. The
// PaperS3 hardware does not preserve esp_sleep_get_wakeup_cause across
// the PMS150 power-button reset, so we cannot rely on the standard
// ESP-IDF wake-cause API; persisted NVS state is the only way to know
// the previous shutdown was an intentional sleep.
//
// Boot sequence:
//   1. mainTask calls SessionState::init_at_boot() very early.
//   2. The function reads the persisted marker, caches the result in
//      a process-local static, and immediately clears the persisted
//      marker so a crash during the rest of boot defaults the next
//      attempt to a safe cold-boot path.
//   3. Any later boot-path code calls SessionState::is_warm_wake() to
//      decide whether to take a fast-path optimization.
//
// Sleep entry sequence:
//   1. Application code paths that intentionally enter deep sleep
//      (Menu → Power Off, idle-timeout escalation, etc.) call
//      SessionState::mark_entering_deep_sleep() before
//      InkPlatePlatform::deep_sleep().
//   2. The hardware-failure deep-sleep paths in main.cpp do NOT mark,
//      so the next boot from a hardware-failure shutdown is treated
//      as a cold boot and runs full init / shows the error UX cleanly.

#pragma once
#include "global.hpp"

#if EPUB_INKPLATE_BUILD

namespace SessionState {

  /// Read the persisted deep-sleep marker, cache it for this boot,
  /// and clear the persisted marker. Call once, very early in main.
  void init_at_boot();

  /// True if this boot is a clean wake from a deliberate deep_sleep.
  /// Always returns false until init_at_boot() has been called.
  bool is_warm_wake();

  /// Persist the deep-sleep marker so the next boot can take the
  /// warm-wake fast path. Call from clean sleep entry points only —
  /// hardware-failure deep-sleep paths must NOT call this so the
  /// recovery boot runs full init.
  void mark_entering_deep_sleep();

}

#endif // EPUB_INKPLATE_BUILD
