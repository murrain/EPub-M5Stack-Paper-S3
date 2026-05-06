// Copyright (c) 2026 Patrick Smith
//
// MIT License. Look at file licenses.txt for details.

#pragma once
#include "global.hpp"

#if EPUB_INKPLATE_BUILD

namespace SleepScreenViewer {

  /// Render a "sleeping" screen with the currently-open book and wake
  /// instructions, then push it to the e-paper display. The image is
  /// retained on the panel without power, so this is the persistent
  /// visible state for the duration of deep sleep.
  ///
  /// Must be called before InkPlatePlatform::deep_sleep(). Safe to
  /// call when no book is open (renders a generic sleeping page).
  void show();

}

#endif
