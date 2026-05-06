#define __SCREEN__ 1
#include "screen.hpp"

#if defined(BOARD_TYPE_PAPER_S3)

#include <cstring>
#include "esp_heap_caps.h"

extern "C" {
  #include <epdiy.h>
  #include <epd_highlevel.h>
  #include <epd_display.h>
}

#ifdef EPD_LOG_UPDATE_TIMING
  #include "esp_timer.h"
  #include "esp_log.h"
#endif

// Board definition implemented in PaperS3Support/EpdiyPaperS3Board.c
extern "C" {
  extern const EpdBoardDefinition paper_s3_board;
}

#ifndef EPD_WIDTH
#define EPD_WIDTH 960
#endif

#ifndef EPD_HEIGHT
#define EPD_HEIGHT 540
#endif

static EpdiyHighlevelState s_hl;
static bool s_epd_initialized = false;
static uint8_t *s_framebuffer = nullptr;
static bool s_force_full = true;
static int16_t s_partial_count = 0;
static const int16_t PARTIAL_COUNT_ALLOWED = 10;
static int s_temperature = 20; // TODO: hook real temperature sensor

// True between a warm-wake screen.setup and the first content
// update(). Triggers an epd_fullclear inside the s_force_full branch
// of update() to flush the wallpaper / sleep-screen retention before
// the new content paints. Cleared on the same update() call that
// consumes it. See the comment at the consumption site for why a
// plain GC16(forced) wasn't enough.
static bool s_warm_wake_clear_pending = false;

Screen Screen::singleton;

uint16_t Screen::width  = EPD_WIDTH;
uint16_t Screen::height = EPD_HEIGHT;

void Screen::clear()
{
  if (!s_epd_initialized) return;
  epd_hl_set_all_white(&s_hl);
}

// The bool shim Screen::update(bool no_full) is defined inline in screen.hpp.
// Only the out-of-line typed entry point lives in this translation unit.

// Cost charged against the partial budget per FAST update. MODE_DU leaves
// more residual ghosting than GL16, so the next forced GC16 cleanup arrives
// sooner. Tuned conservatively at 2 — adjust if ghosting accumulates faster
// or slower in the field.
static constexpr int16_t FAST_BUDGET_COST = 2;

static inline void run_update(EpdDrawMode mode, int temperature, const char * mode_name)
{
  (void)mode_name;
#ifdef EPD_LOG_UPDATE_TIMING
  int64_t t0 = esp_timer_get_time();
#endif
  epd_hl_update_screen(&s_hl, mode, temperature);
#ifdef EPD_LOG_UPDATE_TIMING
  int64_t dt_us = esp_timer_get_time() - t0;
  ESP_LOGI("screen", "update %s: %lld us", mode_name, (long long)dt_us);
#endif
}

void Screen::update(UpdateMode mode)
{
  if (!s_epd_initialized) return;

  // s_force_full always wins, regardless of caller's requested mode. This
  // preserves the historical short-circuit semantics: a pending forced
  // full update cannot be skipped by an explicit FORCE_PARTIAL/FAST.
  if (s_force_full) {
    if (s_warm_wake_clear_pending) {
      s_warm_wake_clear_pending = false;
      // Wallpaper / sleep-screen retention surviving deep sleep
      // does NOT clear with a single MODE_GC16 update. epdiy's HL
      // tracks "previous panel state" and computes a delta against
      // it; on warm wake that tracking starts at all-white (from
      // epd_hl_set_all_white in setup) but the actual panel cells
      // hold the wallpaper, so GC16's white->content waveform
      // can't unwind cells that were never tracked as anything
      // other than white. The user sees the wallpaper bleeding
      // through the rendered page.
      //
      // Fix: drive the panel through the full clearing sequence
      // so it actually matches the HL's all-white tracking, then
      // paint the new content from a real white baseline. The
      // application has already drawn the new content into
      // s_framebuffer by the time we land here (update() is the
      // commit point), so we save it, scrub, and restore.
      const size_t fb_size = (EPD_WIDTH / 2) * EPD_HEIGHT;
      uint8_t * fb_save = (uint8_t *) heap_caps_aligned_alloc(
        16, fb_size, MALLOC_CAP_SPIRAM);
      if (fb_save != nullptr) {
        memcpy(fb_save, s_framebuffer, fb_size);
        epd_hl_set_all_white(&s_hl);
        epd_fullclear(&s_hl, s_temperature);
        memcpy(s_framebuffer, fb_save, fb_size);
        free(fb_save);
      }
      // If the PSRAM alloc fails (extremely unlikely with our
      // current heap state), fall through to a plain GC16. The
      // user will still see ghosting on this single transition,
      // but the device stays up — better than a hard failure on
      // the first warm-wake paint.
    }
    run_update(MODE_GC16, s_temperature, "GC16(forced)");
    s_force_full = false;
    s_partial_count = PARTIAL_COUNT_ALLOWED;
    return;
  }

  switch (mode) {
    case UpdateMode::FULL:
      run_update(MODE_GC16, s_temperature, "GC16(full)");
      s_partial_count = PARTIAL_COUNT_ALLOWED;
      return;

    case UpdateMode::FORCE_PARTIAL:
      run_update(MODE_GL16, s_temperature, "GL16(partial)");
      s_partial_count = 0;
      return;

    case UpdateMode::FAST: {
      // Pre-check the budget so a single update is emitted per call —
      // either the fast waveform or a GC16 cleanup, never both.
#ifdef EPD_FAST_PAGE_TURNS_FALLBACK
      // Fallback: route FAST to GL16 instead of MODE_DU.
      const EpdDrawMode  fast_mode  = MODE_GL16;
      const char * const fast_label = "GL16(fast-fallback)";
#else
      const EpdDrawMode  fast_mode  = MODE_DU;
      const char * const fast_label = "DU(fast)";
#endif
      if (s_partial_count - FAST_BUDGET_COST < 0) {
        run_update(MODE_GC16, s_temperature, "GC16(fast-cleanup)");
        s_partial_count = PARTIAL_COUNT_ALLOWED;
      } else {
        run_update(fast_mode, s_temperature, fast_label);
        s_partial_count -= FAST_BUDGET_COST;
      }
      return;
    }

    case UpdateMode::BUDGETED:
    default:
      if (s_partial_count <= 0) {
        run_update(MODE_GC16, s_temperature, "GC16(budgeted)");
        s_partial_count = PARTIAL_COUNT_ALLOWED;
      } else {
        run_update(MODE_GL16, s_temperature, "GL16(budgeted)");
        s_partial_count--;
      }
      return;
  }
}

void Screen::force_full_update()
{
  s_force_full = true;
  s_partial_count = 0;
}

void Screen::panel_clear()
{
  if (!s_epd_initialized) return;
  epd_hl_set_all_white(&s_hl);
  epd_fullclear(&s_hl, s_temperature);
}

uint8_t * Screen::get_framebuffer_for_snapshot()
{
  return s_epd_initialized ? s_framebuffer : nullptr;
}

size_t Screen::get_framebuffer_size_for_snapshot()
{
  return s_epd_initialized ? (size_t)((EPD_WIDTH / 2) * EPD_HEIGHT) : 0;
}

void Screen::setup(PixelResolution resolution, Orientation orientation,
                   bool preserve_panel_image)
{
  if (!s_epd_initialized) {
    epd_set_board(&paper_s3_board);
    epd_init(epd_current_board(), &ED047TC2, EPD_OPTIONS_DEFAULT);
    // Rotate the epdiy drawing coordinates so that the logical page is
    // portrait when the device is held with USB-C at the bottom and the
    // power button on the right.
    epd_set_rotation(EPD_ROT_INVERTED_PORTRAIT);
    // The C fallback for the ESP32-S3 LUT path is slower than the original
    // vector assembly, so we run the LCD at 5 MHz for stability.
    epd_set_lcd_pixel_clock_MHz(5);

    s_hl = epd_hl_init(EPD_BUILTIN_WAVEFORM);
    epd_hl_set_all_white(&s_hl);
    s_framebuffer = epd_hl_get_framebuffer(&s_hl);

    epd_poweron();
    if (!preserve_panel_image) {
      // Cold-boot path: drive the panel through its full clearing
      // waveform so we start from a known white state regardless of
      // what was last latched on the cells. After fullclear the
      // panel matches the all-white framebuffer and there is no
      // ghosting to flush, so s_force_full stays false.
      epd_fullclear(&s_hl, s_temperature);
    }
    // Warm-wake path: skip the ~700 ms epd_fullclear so the
    // sleep-screen / wallpaper drawn before deep sleep stays
    // visible during the rest of boot. The first real render
    // (book page, books-dir, etc.) replaces it with a single
    // GC16 update, so the user sees one transition instead of
    // black -> splash -> book.
    //
    // We also flag s_warm_wake_clear_pending so that first GC16
    // is preceded by a proper epd_fullclear inside update().
    // Without that, the wallpaper retention bleeds through the
    // rendered page — GC16 alone is a delta waveform against
    // the HL's tracked frame state (all-white), not the actual
    // panel state (wallpaper). Doing the clear at first-render
    // time (rather than here) keeps the wallpaper visible for
    // the whole boot phase and consolidates the visual cost
    // into a single transition.
    s_force_full              = preserve_panel_image;
    s_warm_wake_clear_pending = preserve_panel_image;
    s_epd_initialized         = true;
    s_partial_count           = PARTIAL_COUNT_ALLOWED;
  }

  // On Paper S3 we always drive the panel in grayscale (4-bit via epdiy).
  // Ignore the stored pixel resolution setting and force the non-ONE_BIT
  // path so glyphs/bitmaps are generated as grayscale only.
  (void)resolution;
  set_pixel_resolution(PixelResolution::THREE_BITS, true);
  set_orientation(orientation);
  if (!preserve_panel_image) {
    // clear() resets the framebuffer to all-white. On warm wake we
    // intentionally skip this so the panel image stays in sync with
    // the framebuffer (both still hold the sleep-screen content)
    // until the first real render overwrites them.
    clear();
  }
}

void Screen::set_pixel_resolution(PixelResolution resolution, bool force)
{
  if (force || (pixel_resolution != resolution)) {
    pixel_resolution = resolution;
  }
}

void Screen::set_orientation(Orientation orient)
{
  orientation = orient;
  // With EPD_ROT_INVERTED_PORTRAIT set at init time, epdiy exposes a
  // logical portrait space of 540x960 (EPD_HEIGHT x EPD_WIDTH). Keep the
  // logical Screen dimensions fixed to that space regardless of the
  // orientation enum so the layout engine can use the full page.
  width  = EPD_HEIGHT;  // 540
  height = EPD_WIDTH;   // 960
}

static inline uint8_t map_gray(uint8_t v)
{
  // Convert an 8-bit grayscale value (0=black..255=white) into an epdiy
  // API color byte (upper nibble significant).
  return (uint8_t)(v & 0xF0);
}

static inline uint8_t gray8_to_nibble(uint8_t v)
{
  // 0 (black) .. 255 (white) => 0..15
  return (uint8_t)(v >> 4);
}

static inline uint8_t alpha8_to_nibble(uint8_t a)
{
  // Alpha 0 (transparent) .. 255 (opaque) => 15..0 (white..black).
  // 2x boost so antialiased glyph edges land in the framebuffer's
  // "black" half (nibble <= 7) and survive 2-level (MODE_DU) waveform
  // thresholding without losing visual weight. With a linear mapping,
  // most AA pixels (alpha < 128) thresholded to white under DU,
  // making text appear thin under the FAST update mode.
  uint16_t boosted = (uint16_t)a << 1;
  if (boosted > 255) boosted = 255;
  return (uint8_t)(15 - (boosted >> 4));
}

static inline uint8_t gray3_to_nibble(uint8_t v)
{
  // 3-bit grayscale 0..7 => 0..15
  return (uint8_t)((v * 15 + 3) / 7);
}

static inline void set_pixel_nibble_physical(uint16_t x, uint16_t y, uint8_t nibble)
{
  // Write a 4-bpp pixel directly into the epdiy framebuffer.
  // x: 0..EPD_WIDTH-1 (960), y: 0..EPD_HEIGHT-1 (540)
  uint8_t * buf_ptr = &s_framebuffer[y * (EPD_WIDTH / 2) + (x >> 1)];
  if (x & 1) {
    *buf_ptr = (uint8_t)((*buf_ptr & 0x0F) | ((nibble & 0x0F) << 4));
  } else {
    *buf_ptr = (uint8_t)((*buf_ptr & 0xF0) | (nibble & 0x0F));
  }
}

static inline void set_pixel_nibble_screen(uint16_t x, uint16_t y, uint8_t nibble)
{
  // Screen coordinates for Paper S3 are logical portrait (width=540, height=960)
  // with epdiy set to EPD_ROT_INVERTED_PORTRAIT.
  // The equivalent physical coordinates in the 960x540 framebuffer are:
  //   x_phys = y
  //   y_phys = (EPD_HEIGHT - 1) - x
  set_pixel_nibble_physical(y, (uint16_t)((EPD_HEIGHT - 1) - x), nibble);
}

// Screen-coordinate hot loops: with EPD_ROT_INVERTED_PORTRAIT the
// screen-X axis maps to physical-Y (rows) in the framebuffer, and the
// screen-Y axis maps to physical-X (columns within a row). The
// previous implementation called set_pixel_nibble_screen() per pixel,
// which recomputed `phys_row * (EPD_WIDTH/2)` on every glyph pixel
// and walked through one extra inline call. For a typical book page
// (~1500-3000 glyphs × ~10×15 pixels each) that's hundreds of
// thousands of redundant multiplies/branches. Hoist the per-row base
// pointer out of the inner loop so the inner step is just one address
// add and one read-modify-write per pixel.

void Screen::draw_bitmap(const unsigned char * bitmap_data, Dim dim, Pos pos)
{
  if (!s_epd_initialized || (bitmap_data == nullptr)) return;

  uint16_t x_max = pos.x + dim.width;
  uint16_t y_max = pos.y + dim.height;

  if (x_max > width)  x_max = width;
  if (y_max > height) y_max = height;

  for (uint16_t x = pos.x; x < x_max; ++x) {
    // Hoisted: physical row for this screen-X column. Every (x, *)
    // pixel writes into the same framebuffer row — recomputing the
    // row index per pixel was the dominant cost.
    const uint16_t phys_row = (uint16_t)((EPD_HEIGHT - 1) - x);
    uint8_t * const row_base = &s_framebuffer[phys_row * (EPD_WIDTH / 2)];
    const uint16_t x_off_in_bmp = (uint16_t)(x - pos.x);

    for (uint16_t y = pos.y; y < y_max; ++y) {
      const uint32_t p = (uint32_t)(y - pos.y) * dim.width + x_off_in_bmp;
      const uint8_t  nib = gray8_to_nibble(bitmap_data[p]);
      const uint16_t phys_col = y;
      uint8_t * const buf_ptr = row_base + (phys_col >> 1);
      if (phys_col & 1) {
        *buf_ptr = (uint8_t)((*buf_ptr & 0x0F) | ((nib & 0x0F) << 4));
      } else {
        *buf_ptr = (uint8_t)((*buf_ptr & 0xF0) | (nib & 0x0F));
      }
    }
  }
}

void Screen::draw_glyph(const unsigned char * bitmap_data, Dim dim, Pos pos, uint16_t pitch)
{
  if (!s_epd_initialized || (bitmap_data == nullptr)) return;

  uint16_t x_max = pos.x + dim.width;
  uint16_t y_max = pos.y + dim.height;

  if (x_max > width)  x_max = width;
  if (y_max > height) y_max = height;

  // Glyph buffer is 8-bit alpha (0=transparent..255=opaque). We draw it as
  // black with intensity proportional to alpha.
  for (uint16_t i = 0; i < dim.width && (pos.x + i) < x_max; ++i) {
    // Hoist the row-base pointer for this column out of the inner
    // loop. The previous implementation recomputed
    //   buf_ptr = &s_framebuffer[((EPD_HEIGHT-1)-(pos.x+i)) * (EPD_WIDTH/2) + (...>>1)]
    // for every glyph pixel, paying a multiply + an outer-coord
    // subtraction per pixel. Now the inner loop is just one address
    // add (phys_col>>1) and the nibble RMW.
    const uint16_t phys_row = (uint16_t)((EPD_HEIGHT - 1) - (pos.x + i));
    uint8_t * const row_base = &s_framebuffer[phys_row * (EPD_WIDTH / 2)];
    const unsigned char * const col_base = bitmap_data + i;

    for (uint16_t j = 0; j < dim.height && (pos.y + j) < y_max; ++j) {
      const uint8_t a = col_base[j * pitch];
      if (!a) continue;
      const uint8_t nib = alpha8_to_nibble(a);
      if (nib == 0x0F) continue;
      const uint16_t phys_col = (uint16_t)(pos.y + j);
      uint8_t * const buf_ptr = row_base + (phys_col >> 1);
      if (phys_col & 1) {
        *buf_ptr = (uint8_t)((*buf_ptr & 0x0F) | ((nib & 0x0F) << 4));
      } else {
        *buf_ptr = (uint8_t)((*buf_ptr & 0xF0) | (nib & 0x0F));
      }
    }
  }
}

void Screen::draw_rectangle(Dim dim, Pos pos, uint8_t color)
{
  if (!s_epd_initialized) return;

  uint16_t x_max = pos.x + dim.width;
  uint16_t y_max = pos.y + dim.height;
  if (x_max > width)  x_max = width;
  if (y_max > height) y_max = height;
  if (x_max <= pos.x || y_max <= pos.y) return;

  const uint8_t nib = gray3_to_nibble(color);

  // Top and bottom edges
  for (uint16_t x = pos.x; x < x_max; ++x) {
    set_pixel_nibble_screen(x, pos.y, nib);
    set_pixel_nibble_screen(x, (uint16_t)(y_max - 1), nib);
  }
  // Left and right edges
  for (uint16_t y = pos.y; y < y_max; ++y) {
    set_pixel_nibble_screen(pos.x, y, nib);
    set_pixel_nibble_screen((uint16_t)(x_max - 1), y, nib);
  }
}

void Screen::draw_round_rectangle(Dim dim, Pos pos, uint8_t color)
{
  // Approximate with a simple rectangle for now.
  draw_rectangle(dim, pos, color);
}

void Screen::colorize_region(Dim dim, Pos pos, uint8_t color)
{
  if (!s_epd_initialized) return;

  uint16_t x_max = pos.x + dim.width;
  uint16_t y_max = pos.y + dim.height;
  if (x_max > width)  x_max = width;
  if (y_max > height) y_max = height;
  if (x_max <= pos.x || y_max <= pos.y) return;

  const uint8_t nib = gray3_to_nibble(color);

  for (uint16_t x = pos.x; x < x_max; ++x) {
    for (uint16_t y = pos.y; y < y_max; ++y) {
      set_pixel_nibble_screen(x, y, nib);
    }
  }
}

#endif // BOARD_TYPE_PAPER_S3
