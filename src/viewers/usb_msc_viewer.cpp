// Copyright (c) 2026 Patrick Smith
//
// MIT License. Look at file licenses.txt for details.

#include "viewers/usb_msc_viewer.hpp"

#if EPUB_INKPLATE_BUILD && defined(BOARD_TYPE_PAPER_S3)

#include "viewers/centered_layout.hpp"
#include "viewers/page.hpp"
#include "models/fonts.hpp"
#include "screen.hpp"
#include "logging.hpp"

#include <cstdio>
#include <string>

namespace
{
  static constexpr char const * TAG = "UsbMscViewer";

  // line_height_factor for this viewer's text. The shared
  // CenteredLayout::make_fmt takes it as a parameter; sleep_screen
  // uses 1.1, this one uses 1.15. Both values were tuned by hand
  // for their respective content density; keep them per-viewer.
  constexpr float LINE_HEIGHT_FACTOR = 1.15f;

  inline Page::Format make_fmt(int8_t font_index, int16_t font_size,
                               Fonts::FaceStyle style, CSS::Align align)
  {
    return CenteredLayout::make_fmt(font_index, font_size, style, align,
                                    LINE_HEIGHT_FACTOR);
  }
  using CenteredLayout::put_centered;
  using CenteredLayout::draw_hrule;
}

void UsbMscViewer::show()
{
  if (page.get_compute_mode() == Page::ComputeMode::LOCATION) {
    LOG_W("UsbMscViewer::show called during page-locations compute; skipping");
    return;
  }

  page.set_compute_mode(Page::ComputeMode::DISPLAY);

  const int16_t W = Screen::get_width();
  const int16_t H = Screen::get_height();

  Page::Format fmt = make_fmt(1, 14, Fonts::FaceStyle::NORMAL, CSS::Align::CENTER);
  page.start(fmt);

  // Drive a full panel-clear waveform so we leave no ghost of the
  // previous content. The MSC banner is the persistent visible
  // state for the entire duration of the USB session, which can
  // be many minutes — start from a clean white background.
  screen.panel_clear();
  page.clear_region(Dim((uint16_t)W, (uint16_t)H), Pos(0, 0));

  // Decorative rules, mirroring the sleep-screen layout vocabulary.
  draw_hrule(W, 90, 0, 2);
  draw_hrule(W, H - 90, 0, 2);

  // ── Title ────────────────────────────────────────────────────────────
  {
    Page::Format title_fmt = make_fmt(1, 36, Fonts::FaceStyle::BOLD, CSS::Align::CENTER);
    put_centered("USB Drive Mode", 200, title_fmt);
  }

  // ── Body lines ───────────────────────────────────────────────────────
  {
    Page::Format body_fmt = make_fmt(1, 14, Fonts::FaceStyle::NORMAL, CSS::Align::CENTER);
    put_centered("The microSD card is now visible to your computer", 320, body_fmt);
    put_centered("as a removable USB drive.", 348, body_fmt);
  }

  {
    Page::Format italic_fmt = make_fmt(1, 13, Fonts::FaceStyle::ITALIC, CSS::Align::CENTER);
    put_centered("Drag-and-drop EPUBs into /books, wallpapers into /wallpapers,", 410, italic_fmt);
    put_centered("or browse any other file on the card.", 432, italic_fmt);
  }

  draw_hrule(W, 510, 80);

  // ── Safe-exit instructions ───────────────────────────────────────────
  {
    Page::Format hint_fmt = make_fmt(1, 13, Fonts::FaceStyle::NORMAL, CSS::Align::CENTER);
    put_centered("To exit:", 560, hint_fmt);
    put_centered("1. Eject the drive from your computer", 590, hint_fmt);
    put_centered("2. Tap anywhere on this screen", 615, hint_fmt);
    put_centered("(the device will restart)", 640, hint_fmt);
  }

  // Force a full GC16 refresh so the long-retention banner is
  // ghost-free for the entire connected session.
  screen.force_full_update();
  page.paint(false);
}

#endif
