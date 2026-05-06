// Copyright (c) 2026 Patrick Smith
//
// MIT License. Look at file licenses.txt for details.

#pragma once
#include "global.hpp"

#if EPUB_INKPLATE_BUILD

namespace WallpaperViewer {

  /// Decode a random PNG from dir_path and blit it full-screen as an
  /// 8-bit grayscale image.  Returns true if a painting was shown, false
  /// if the directory is missing or contains no PNG files.
  /// If a sidecar .txt exists alongside the PNG its metadata is stored and
  /// accessible via last_title() / last_artist_date() / last_blurb().
  bool show_random(const char * dir_path = WALLPAPERS_FOLDER);

  /// Metadata for the most recently shown painting (empty strings if none).
  const char * last_title();
  const char * last_artist_date();
  const char * last_blurb();

}

#endif
