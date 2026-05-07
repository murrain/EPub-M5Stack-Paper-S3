// Copyright (c) 2026 Patrick Smith
//
// MIT License. Look at file licenses.txt for details.

#include "viewers/sleep_screen_viewer.hpp"

#if EPUB_INKPLATE_BUILD

#include "viewers/wallpaper_viewer.hpp"
#include "viewers/centered_layout.hpp"
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
  using CenteredLayout::PAD;            // shared text left/right margin
  using CenteredLayout::put_centered;
  using CenteredLayout::draw_hrule;

  constexpr int16_t F_OUTER = 14;       // outer border inset from screen edge
  constexpr int16_t F_INNER = 24;       // inner border inset

  // line_height_factor for this viewer's text. The shared
  // CenteredLayout::make_fmt takes it as a parameter; usb_msc uses
  // 1.15, this viewer uses 1.1. Both values were tuned by hand
  // for their respective content density; keep them per-viewer.
  constexpr double LINE_HEIGHT_FACTOR = 1.1;

  inline Page::Format make_fmt(int8_t font_index, int16_t font_size,
                               Fonts::FaceStyle style, CSS::Align align)
  {
    return CenteredLayout::make_fmt(font_index, font_size, style, align,
                                    LINE_HEIGHT_FACTOR);
  }

  // Split `text` at a word boundary near `max_chars`.
  void word_split(const char * text, std::string & first, std::string & second,
                  int max_chars = 65)
  {
    size_t len = strlen(text);
    if ((int)len <= max_chars) { first = text; second = ""; return; }
    int split = max_chars;
    while (split > 0 && text[split] != ' ') split--;
    if (split == 0) split = max_chars;
    first = std::string(text, (size_t)split);
    const char * rest = text + split;
    while (*rest == ' ') rest++;
    second = rest;
    if ((int)second.size() > max_chars) {
      size_t cut = (size_t)max_chars;
      while (cut > 0 && second[cut] != ' ') cut--;
      if (cut == 0) cut = (size_t)max_chars;
      second = second.substr(0, cut) + "...";
    }
  }

  // 3-px thick outer border built from four filled strips.
  void draw_outer_frame(int16_t W, int16_t H)
  {
    constexpr int16_t T  = 3;
    int16_t inner_w = W - 2*F_OUTER;
    int16_t inner_h = H - 2*F_OUTER;
    screen.colorize_region(Dim(inner_w, T), Pos(F_OUTER, F_OUTER),         Screen::BLACK_COLOR);
    screen.colorize_region(Dim(inner_w, T), Pos(F_OUTER, H-F_OUTER-T),     Screen::BLACK_COLOR);
    screen.colorize_region(Dim(T, inner_h), Pos(F_OUTER, F_OUTER),         Screen::BLACK_COLOR);
    screen.colorize_region(Dim(T, inner_h), Pos(W-F_OUTER-T, F_OUTER),     Screen::BLACK_COLOR);
  }

  // Dot-grid ornament: COLS×ROWS of 2×2 filled squares on an 8-px pitch.
  // GH = (ROWS-1)*8 + 2 = 66 px tall for ROWS=9.
  void draw_dot_grid(int16_t W, int16_t top_y)
  {
    constexpr int16_t DOT   = 2;
    constexpr int16_t PITCH = 8;
    constexpr int16_t COLS  = 11;
    constexpr int16_t ROWS  = 9;
    constexpr int16_t GW    = (COLS - 1)*PITCH + DOT;  // 82 px
    int16_t gx = (W - GW) / 2;
    for (int16_t r = 0; r < ROWS; r++) {
      for (int16_t c = 0; c < COLS; c++) {
        screen.colorize_region(Dim(DOT, DOT),
                               Pos(gx + c*PITCH, top_y + r*PITCH),
                               Screen::BLACK_COLOR);
      }
    }
  }
}

// Show a battery warning only when charge is critically low.
// Returns true if a warning was rendered.
static bool draw_battery_if_critical(int16_t y)
{
  #if defined(BOARD_TYPE_PAPER_S3)
  {
    int  pct = battery.read_percentage();
    bool usb = battery.is_usb_powered();
    if (!usb && pct < 10) {
      char buf[48];
      snprintf(buf, sizeof(buf), "Battery critical: %d%% - please charge", pct);
      Page::Format fmt = make_fmt(1, 8, Fonts::FaceStyle::NORMAL, CSS::Align::CENTER);
      put_centered(buf, y, fmt);
      return true;
    }
  }
  #endif
  return false;
}

// ── Wallpaper caption strip: title + artist · date ───────────────────────────
static void draw_wallpaper_caption(int16_t W, int16_t H)
{
  constexpr int16_t STRIP_H   = 90;
  const     int16_t strip_top = H - STRIP_H;    // 870

  screen.colorize_region(Dim((uint16_t)W, (uint16_t)STRIP_H),
                         Pos(0, strip_top), Screen::WHITE_COLOR);
  screen.colorize_region(Dim((uint16_t)W, 2), Pos(0, strip_top), Screen::BLACK_COLOR);

  // Title — font size and line count scale with length.
  // Empirically ~20 chars fits per line at 14pt; ~28 chars at 11pt.
  int16_t artist_y = strip_top + 55;  // default: single-line title
  const char * pt = WallpaperViewer::last_title();
  if (pt && pt[0]) {
    const int len = (int)strlen(pt);
    std::string t1, t2;
    int16_t title_size;

    if (len <= 20) {
      title_size = 14;
      t1 = pt;
    } else if (len <= 40) {
      title_size = 14;
      word_split(pt, t1, t2, 20);
    } else {
      title_size = 11;
      t1 = pt;
    }

    Page::Format fmt = make_fmt(1, title_size, Fonts::FaceStyle::ITALIC, CSS::Align::CENTER);
    if (t2.empty()) {
      put_centered(t1, strip_top + 31, fmt);
    } else {
      put_centered(t1, strip_top + 22, fmt);
      put_centered(t2, strip_top + 40, fmt);
      artist_y = strip_top + 62;
    }
  }

  const char * ad = WallpaperViewer::last_artist_date();
  if (ad && ad[0]) {
    Page::Format fmt = make_fmt(1, 11, Fonts::FaceStyle::NORMAL, CSS::Align::CENTER);
    put_centered(ad, artist_y, fmt);
  }

  draw_battery_if_critical(artist_y + 18);
}

// ── Geometric-design footer: book progress + battery + wake ──────────────────
static void draw_geometric_footer(int16_t W, int16_t H)
{
  // Rule above footer.
  draw_hrule(W, H - 174, 0);

  // Book title (italic, 13pt)
  {
    const char * raw  = epub.get_title();
    std::string title = (raw && raw[0]) ? raw : "(no book open)";
    Page::Format fmt  = make_fmt(1, 13, Fonts::FaceStyle::ITALIC, CSS::Align::CENTER);
    put_centered(title, H - 148, fmt);
  }

  // Page progress (normal, 12pt)
  {
    char buf[64] = {};
    int16_t total = page_locs.get_page_count();
    int16_t curr  = page_locs.get_page_nbr(book_controller.get_current_page_id());
    if (total > 0 && curr >= 0) {
      int pct = (int)((curr + 1) * 100 / total);
      snprintf(buf, sizeof(buf), "Page %d of %d  •  %d%%", (int)(curr+1), (int)total, pct);
    } else if (curr >= 0) {
      snprintf(buf, sizeof(buf), "Page %d", (int)(curr + 1));
    }
    if (buf[0]) {
      Page::Format fmt = make_fmt(1, 12, Fonts::FaceStyle::NORMAL, CSS::Align::CENTER);
      put_centered(buf, H - 126, fmt);
    }
  }

  draw_battery_if_critical(H - 90);
}

void SleepScreenViewer::show()
{
  static constexpr char const * TAG = "SleepScreenViewer";
  if (page.get_compute_mode() == Page::ComputeMode::LOCATION) {
    LOG_E("SleepScreenViewer::show called during page-locations compute; skipping");
    return;
  }

  page.set_compute_mode(Page::ComputeMode::DISPLAY);

  const int16_t W = Screen::get_width();   // 540
  const int16_t H = Screen::get_height();  // 960

  // Full panel clear: drives the e-ink through its clearing waveform and
  // resets the framebuffer to white. This eliminates book-text ghosting
  // before the painting is drawn; the subsequent GC16 refresh then lands
  // on a truly clean panel.
  screen.panel_clear();

  Page::Format fmt = make_fmt(1, 12, Fonts::FaceStyle::NORMAL, CSS::Align::CENTER);
  page.start(fmt);

  // ── Try a painting wallpaper first ───────────────────────────────────────
  bool has_wallpaper = WallpaperViewer::show_random();

  if (has_wallpaper) {
    draw_wallpaper_caption(W, H);
  } else {
    // ── Geometric lock-screen design ───────────────────────────────────────

    // Picture-frame border: 3-px outer + 1-px inner + corner squares.
    draw_outer_frame(W, H);
    screen.draw_rectangle(Dim(W - 2*F_INNER, H - 2*F_INNER),
                          Pos(F_INNER, F_INNER), Screen::BLACK_COLOR);
    {
      constexpr int16_t CS = 8;
      constexpr int16_t CO = F_INNER - CS/2;
      screen.colorize_region(Dim(CS, CS), Pos(CO,      CO      ), Screen::BLACK_COLOR);
      screen.colorize_region(Dim(CS, CS), Pos(W-CO-CS, CO      ), Screen::BLACK_COLOR);
      screen.colorize_region(Dim(CS, CS), Pos(CO,      H-CO-CS ), Screen::BLACK_COLOR);
      screen.colorize_region(Dim(CS, CS), Pos(W-CO-CS, H-CO-CS ), Screen::BLACK_COLOR);
    }

    // Top dot-grid ornament  (y 48–114, GH = 8*8+2 = 66 px).
    draw_dot_grid(W, 48);

    // "currently reading" subhead.
    {
      Page::Format sub_fmt = make_fmt(1, 14, Fonts::FaceStyle::ITALIC, CSS::Align::CENTER);
      put_centered("currently reading", 162, sub_fmt);
    }
    draw_hrule(W, 178, 0);

    // Book title — hero element.
    {
      const char * raw  = epub.get_title();
      std::string title = (raw && raw[0]) ? raw : "(no book open)";
      Page::Format title_fmt = make_fmt(1, 32, Fonts::FaceStyle::BOLD, CSS::Align::CENTER);
      put_centered(title, 252, title_fmt);
    }

    // Double-rule ornament below title.
    draw_hrule(W, 285, 80);
    draw_hrule(W, 292, 80);

    // Reading progress: text + bar with quarter-tick marks.
    {
      char buf[64] = {};
      int16_t total = page_locs.get_page_count();
      int16_t curr  = page_locs.get_page_nbr(book_controller.get_current_page_id());

      if (total > 0 && curr >= 0) {
        int pct = (int)((curr + 1) * 100 / total);
        snprintf(buf, sizeof(buf), "Page %d of %d  •  %d%%", (int)(curr+1), (int)total, pct);
      } else if (curr >= 0) {
        snprintf(buf, sizeof(buf), "Page %d", (int)(curr + 1));
      }

      if (buf[0]) {
        Page::Format prog_fmt = make_fmt(1, 13, Fonts::FaceStyle::NORMAL, CSS::Align::CENTER);
        put_centered(buf, 328, prog_fmt);

        if (total > 0 && curr >= 0) {
          int16_t bar_x = PAD + 40;
          int16_t bar_w = W - 2*(PAD + 40);
          int16_t bar_y = 348;
          int16_t bar_h = 10;
          screen.draw_rectangle(Dim(bar_w, bar_h), Pos(bar_x, bar_y), Screen::BLACK_COLOR);
          int16_t fill_w = (int16_t)((int32_t)bar_w * (curr + 1) / total);
          if (fill_w > 0)
            screen.colorize_region(Dim(fill_w, bar_h), Pos(bar_x, bar_y), Screen::BLACK_COLOR);
          for (int q = 1; q <= 3; q++) {
            int16_t tx = bar_x + (int16_t)((int32_t)bar_w * q / 4);
            screen.colorize_region(Dim(1, bar_h - 4), Pos(tx, bar_y + 2), Screen::WHITE_COLOR);
          }
        }
      }
    }

    // Bottom dot-grid ornament (y 720–786).
    draw_dot_grid(W, 720);

    draw_geometric_footer(W, H);
  }

  // Full GC16 refresh — clean panel for the long retention period.
  screen.force_full_update();
  page.paint(false);
}

#endif
