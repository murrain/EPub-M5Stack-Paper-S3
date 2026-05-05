// Copyright (c) 2026 Patrick Smith
//
// MIT License. Look at file licenses.txt for details.
//
// Full-screen UI rendered while the device is in USB Drive Mode.
// Shows the user a status banner explaining what's happening and
// how to exit. The screen is rendered once on entry — e-ink retains
// it for the entire duration the host is connected, no per-tick
// updates required.

#pragma once
#include "global.hpp"

#if EPUB_INKPLATE_BUILD

namespace UsbMscViewer {

  /// Render a full-screen "Connected as USB drive — press any key to
  /// exit" status page and push it to the panel via a single GC16
  /// refresh. Blocks until the panel update is committed.
  void show();

}

#endif
