// Copyright (c) 2026 EPub-InkPlate contributors
//
// MIT License. Look at file licenses.txt for details.

#pragma once

#include "global.hpp"

#if EPUB_INKPLATE_BUILD && defined(BOARD_TYPE_PAPER_S3)

#include "models/css.hpp"
#include "models/fonts.hpp"
#include "viewers/page.hpp"

// Helpers shared by the full-screen text viewers (sleep screen, USB
// drive mode). Each viewer paints a centered, multi-line message
// over the full panel; both used to maintain its own private copy of
// `make_fmt` / `put_centered` / `draw_hrule` and the PAD margin
// constant. The only inter-viewer difference was line_height_factor
// (1.1 in sleep_screen, 1.15 in usb_msc), which is now a parameter.
//
// Future full-screen viewers (an "About" / firmware-version screen,
// a fatal-error screen, etc.) should reuse these instead of growing
// a third copy.
namespace CenteredLayout {

  // Text left/right margin shared by both existing call sites.
  constexpr int16_t PAD = 52;

  // Build a Page::Format suitable for centered full-screen text.
  // line_height_factor is the only knob that varied between
  // sleep_screen (1.1) and usb_msc (1.15); pass yours.
  //
  // line_height_factor is `float` (not double) to match Page::
  // Format::line_height_factor exactly — the paper_s3 build runs
  // with -Werror=narrowing, so passing a double through here and
  // assigning it to a float field fails to compile. Float is also
  // sufficient for the actual values used (one decimal of
  // precision).
  Page::Format make_fmt(int8_t           font_index,
                        int16_t          font_size,
                        Fonts::FaceStyle style,
                        CSS::Align       align,
                        float            line_height_factor);

  // Paint `text` centered horizontally at panel-y `y`. Mutates
  // fmt.align to CENTER (the prior behavior of both private
  // copies; preserved so callers don't have to know the detail).
  void put_centered(const std::string & text,
                    int16_t             y,
                    Page::Format &      fmt);

  // Horizontal rule, PAD + inset on each side. Optional thickness.
  // Returns silently if the resulting width is non-positive (panel
  // narrower than 2*(PAD+inset), shouldn't happen on PaperS3 but
  // defensive against future small-panel ports).
  void draw_hrule(int16_t W,
                  int16_t y,
                  int16_t inset,
                  int16_t thickness = 1);

} // namespace CenteredLayout

#endif // EPUB_INKPLATE_BUILD && BOARD_TYPE_PAPER_S3
