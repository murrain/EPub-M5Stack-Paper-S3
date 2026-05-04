// Copyright (c) 2026 Patrick Smith
//
// MIT License. Look at file licenses.txt for details.

#include "viewers/sleep_screen_viewer.hpp"

#if EPUB_INKPLATE_BUILD

#include "viewers/page.hpp"
#include "models/epub.hpp"
#include "models/page_locs.hpp"
#include "models/fonts.hpp"
#include "controllers/book_controller.hpp"
#include "screen.hpp"
#include "logging.hpp"

#if defined(BOARD_TYPE_PAPER_S3)
  #include "battery_paper_s3.hpp"
#endif

#include <cstdio>
#include <string>

namespace
{
  // Margin into which we draw nothing — keeps the layout away from
  // the screen edge regardless of orientation.
  constexpr int16_t SIDE_PADDING = 60;
  constexpr int16_t HORIZ_RULE_THICKNESS = 2;

  Page::Format make_fmt(int8_t font_index,
                        int16_t font_size,
                        Fonts::FaceStyle style,
                        CSS::Align align)
  {
    Page::Format fmt = {
      .line_height_factor = 1.1,
      .font_index         = font_index,
      .font_size          = font_size,
      .indent             = 0,
      .margin_left        = 0,
      .margin_right       = 0,
      .margin_top         = 0,
      .margin_bottom      = 0,
      .screen_left        = SIDE_PADDING,
      .screen_right       = SIDE_PADDING,
      .screen_top         = 0,
      .screen_bottom      = 0,
      .width              = 0,
      .height             = 0,
      .vertical_align     = 0,
      .trim               = true,
      .pre                = false,
      .font_style         = style,
      .align              = align,
      .text_transform     = CSS::TextTransform::NONE,
      .display            = CSS::Display::INLINE
    };
    return fmt;
  }

  /// Draw a thin horizontal rule, centered between SIDE_PADDING and
  /// (Screen::get_width() - SIDE_PADDING). Used as a decorative element.
  void draw_rule(int16_t y, int16_t inset = 0)
  {
    int16_t W = Screen::get_width();
    int16_t left = SIDE_PADDING + inset;
    int16_t width = W - 2 * (SIDE_PADDING + inset);
    if (width <= 0) return;
    screen.colorize_region(Dim((uint16_t)width, HORIZ_RULE_THICKNESS),
                           Pos(left, y),
                           Screen::BLACK_COLOR);
  }

  /// Place a single string centered horizontally at vertical position y
  /// (where y is the text baseline).
  void put_centered(const std::string & text, int16_t y, Page::Format & fmt)
  {
    fmt.align = CSS::Align::CENTER;
    page.put_str_at(text, Pos(Page::HORIZONTAL_CENTER, y), fmt);
  }
}

void SleepScreenViewer::show()
{
  // Don't run while pagination is in flight — the display list belongs
  // to the layout pass in that case.
  if (page.get_compute_mode() == Page::ComputeMode::LOCATION) {
    LOG_W("SleepScreenViewer::show called during page-locations compute; skipping");
    return;
  }

  page.set_compute_mode(Page::ComputeMode::DISPLAY);

  const int16_t W = Screen::get_width();
  const int16_t H = Screen::get_height();

  // White out the canvas. The clear region covers the full screen so
  // any prior content (book page, menu, dialog) is fully replaced by
  // the sleep design.
  Page::Format fmt = make_fmt(/*font_index=*/1, /*font_size=*/12,
                              Fonts::FaceStyle::NORMAL,
                              CSS::Align::CENTER);
  page.start(fmt);
  page.clear_region(Dim((uint16_t)W, (uint16_t)H), Pos(0, 0));

  // Decorative top + bottom rules form a frame around the content.
  draw_rule(80);
  draw_rule(H - 100);

  // ──────────────  Header: "Sleeping"  ──────────────
  {
    Page::Format header_fmt = make_fmt(1, 36,
                                       Fonts::FaceStyle::BOLD,
                                       CSS::Align::CENTER);
    put_centered("Sleeping", 180, header_fmt);
  }

  // Decorative ornament under the header.
  draw_rule(210, /*inset=*/120);

  // ──────────────  Currently reading subhead  ──────────────
  {
    Page::Format sub_fmt = make_fmt(1, 14,
                                    Fonts::FaceStyle::ITALIC,
                                    CSS::Align::CENTER);
    put_centered("Currently reading", 280, sub_fmt);
  }

  // ──────────────  Book title  ──────────────
  {
    const char * raw_title = epub.get_title();
    std::string  title     = (raw_title != nullptr && raw_title[0] != '\0')
                               ? std::string(raw_title)
                               : std::string("(no book open)");

    Page::Format title_fmt = make_fmt(1, 24,
                                      Fonts::FaceStyle::NORMAL,
                                      CSS::Align::CENTER);
    // Use put_str_at for absolute placement; long titles will get
    // truncated by the screen edge (acceptable for v1).
    put_centered(title, 340, title_fmt);
  }

  // ──────────────  Reading progress  ──────────────
  {
    char buf[64] = {};
    int16_t total = page_locs.get_page_count();
    int16_t curr  = page_locs.get_page_nbr(book_controller.get_current_page_id());

    if (total > 0 && curr >= 0) {
      int pct = (int)((curr + 1) * 100 / total);
      snprintf(buf, sizeof(buf), "Page %d of %d  •  %d%%",
               (int)(curr + 1), (int)total, pct);
    } else if (curr >= 0) {
      snprintf(buf, sizeof(buf), "Page %d", (int)(curr + 1));
    } else {
      buf[0] = '\0';
    }

    if (buf[0] != '\0') {
      Page::Format prog_fmt = make_fmt(1, 13,
                                       Fonts::FaceStyle::NORMAL,
                                       CSS::Align::CENTER);
      put_centered(buf, 410, prog_fmt);

      // Progress bar under the text.
      if (total > 0 && curr >= 0) {
        int16_t bar_width  = W - 2 * (SIDE_PADDING + 60);
        int16_t bar_x      = SIDE_PADDING + 60;
        int16_t bar_y      = 430;
        int16_t bar_height = 8;
        // Outline.
        screen.draw_rectangle(Dim((uint16_t)bar_width, (uint16_t)bar_height),
                              Pos(bar_x, bar_y),
                              Screen::BLACK_COLOR);
        // Fill.
        int16_t fill_w = (int16_t)((int32_t)bar_width * (curr + 1) / total);
        if (fill_w > 0) {
          screen.colorize_region(Dim((uint16_t)fill_w, (uint16_t)bar_height),
                                 Pos(bar_x, bar_y),
                                 Screen::BLACK_COLOR);
        }
      }
    }
  }

  // ──────────────  Wake instructions  ──────────────
  {
    Page::Format wake_fmt = make_fmt(1, 14,
                                     Fonts::FaceStyle::NORMAL,
                                     CSS::Align::CENTER);
    put_centered("Press the side button to wake", H - 200, wake_fmt);
  }

  // ──────────────  Battery indicator (PaperS3 only)  ──────────────
  #if defined(BOARD_TYPE_PAPER_S3)
  {
    char buf[32];
    int pct  = battery.read_percentage();
    float v  = battery.read_level();
    bool  usb = battery.is_usb_powered();
    if (v > 0.0f) {
      snprintf(buf, sizeof(buf), "Battery %d%%  (%.2fV)%s",
               pct, v, usb ? "  ⚡" : "");
    } else {
      snprintf(buf, sizeof(buf), "Battery: --");
    }
    Page::Format bat_fmt = make_fmt(1, 11,
                                    Fonts::FaceStyle::NORMAL,
                                    CSS::Align::CENTER);
    put_centered(buf, H - 130, bat_fmt);
  }
  #endif

  // Force a full GC16 refresh so the panel is in a clean state for the
  // long retention period. A partial waveform would leave residual
  // ghosting that becomes more visible after hours / days asleep.
  screen.force_full_update();
  page.paint(false);
}

#endif
