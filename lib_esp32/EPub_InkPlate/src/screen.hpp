// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#pragma once
#include "global.hpp"

#include "non_copyable.hpp"
#include "inkplate_platform.hpp"

/**
 * @brief Low level logical Screen display
 * 
 * This class implements the low level methods required to paint
 * on the display. Under the InkPlate6, it is using the EInk display driver. 
 * For BOARD_TYPE_PAPER_S3, a minimal stub implementation is provided
 * that will later be backed by the epdiy renderer.
 */

#if defined(BOARD_TYPE_PAPER_S3)

class Screen : NonCopyable
{
  public:
    static constexpr uint8_t    BLACK_COLOR =   0;
    static constexpr uint8_t    WHITE_COLOR =   7;
    static constexpr int8_t     IDENT       =   4;
    static constexpr uint16_t   RESOLUTION  = 212;  ///< Approximate pixels per inch

    enum class Orientation     : int8_t { LEFT, RIGHT, BOTTOM, TOP };
    enum class PixelResolution : int8_t { ONE_BIT, THREE_BITS };

    /**
     * @brief Screen update waveform-mode discipline.
     *
     * Selects which epdiy waveform the renderer wants for a given paint.
     * BUDGETED is the historical default (GL16 with periodic forced GC16
     * cleanups). FAST routes to MODE_DU on PaperS3 for fast text page turns.
     */
    enum class UpdateMode : int8_t {
      BUDGETED,       ///< Default behavior: GL16 with budget-driven forced GC16 (was no_full=false)
      FORCE_PARTIAL,  ///< Always GL16, resets budget (was no_full=true)
      FULL,           ///< Always GC16, resets budget
      FAST,           ///< PaperS3: MODE_DU. Inkplate: partial_update fallback (no MODE_DU available)
    };

    void          draw_bitmap(const unsigned char * bitmap_data, Dim dim, Pos pos);
    void           draw_glyph(const unsigned char * bitmap_data, Dim dim, Pos pos, uint16_t pitch);
    void       draw_rectangle(Dim dim, Pos pos, uint8_t color);
    void draw_round_rectangle(Dim dim, Pos pos, uint8_t color);
    void      colorize_region(Dim dim, Pos pos, uint8_t color);

    void clear();
    void update(UpdateMode mode);
    inline void update(bool no_full = false) {
      update(no_full ? UpdateMode::FORCE_PARTIAL : UpdateMode::BUDGETED);
    }

  private:
    static constexpr char const * TAG = "Screen";

    static uint16_t width;
    static uint16_t height;

    static Screen singleton;
    Screen() : pixel_resolution(PixelResolution::ONE_BIT),
               orientation(Orientation::BOTTOM) { };

    PixelResolution   pixel_resolution;
    Orientation       orientation;

  public:
    static Screen & get_singleton() noexcept { return singleton; }
    /// Initialize the e-paper hardware. When `preserve_panel_image` is
    /// true the boot-time epd_fullclear (a ~700ms clearing waveform that
    /// drives the panel back to all-white) and the framebuffer wipe are
    /// both skipped, leaving whatever image is currently latched on the
    /// physical panel intact. Used on warm wake from deep sleep so the
    /// sleep-screen / wallpaper drawn before sleep stays visible during
    /// boot until the application replaces it with real content.
    void setup(PixelResolution resolution, Orientation orientation,
               bool preserve_panel_image = false);
    void set_pixel_resolution(PixelResolution resolution, bool force = false);
    void set_orientation(Orientation orient);
    inline Orientation get_orientation() { return orientation; }
    inline PixelResolution get_pixel_resolution() { return pixel_resolution; }
    void force_full_update();
    // Drive the panel through its full-clear waveform and reset the
    // framebuffer to all-white. Use before sleep-screen rendering to
    // eliminate residual ghosting from the previous book page.
    void panel_clear();

    inline static uint16_t get_width() { return width; }
    inline static uint16_t get_height() { return height; }
};

#else  // !BOARD_TYPE_PAPER_S3

class Screen : NonCopyable
{
  public:
    static constexpr uint8_t    BLACK_COLOR           =   0;
    static constexpr uint8_t    WHITE_COLOR           =   7;
    #if INKPLATE_10
      static constexpr int8_t   IDENT                 =   2;
      static constexpr int8_t   PARTIAL_COUNT_ALLOWED =  10;
      static constexpr uint16_t RESOLUTION            = 150;  ///< Pixels per inch
    #elif INKPLATE_6
      static constexpr int8_t   IDENT                 =   1;
      static constexpr int8_t   PARTIAL_COUNT_ALLOWED =  10;
      static constexpr uint16_t RESOLUTION            = 166;  ///< Pixels per inch
    #elif INKPLATE_6PLUS
      static constexpr int8_t   IDENT                 =   3;
      static constexpr int16_t  PARTIAL_COUNT_ALLOWED =  10;
      static constexpr uint16_t RESOLUTION            = 212;  ///< Pixels per inch
    #endif
    enum class Orientation     : int8_t { LEFT, RIGHT, BOTTOM, TOP };
    enum class PixelResolution : int8_t { ONE_BIT, THREE_BITS };

    /**
     * @brief Screen update waveform-mode discipline.
     *
     * Selects which waveform the renderer wants for a given paint. BUDGETED
     * is the historical default (partial updates with periodic forced full
     * cleanups). FAST has no MODE_DU equivalent on Inkplate hardware, so it
     * falls back to a partial update.
     */
    enum class UpdateMode : int8_t {
      BUDGETED,       ///< Default behavior: partial with budget-driven forced full (was no_full=false)
      FORCE_PARTIAL,  ///< Always partial, resets budget (was no_full=true)
      FULL,           ///< Always full update, resets budget
      FAST,           ///< Inkplate: same as FORCE_PARTIAL (no MODE_DU available)
    };

    void          draw_bitmap(const unsigned char * bitmap_data, Dim dim, Pos pos);
    void           draw_glyph(const unsigned char * bitmap_data, Dim dim, Pos pos, uint16_t pitch);
    void       draw_rectangle(Dim dim, Pos pos, uint8_t color);
    void draw_round_rectangle(Dim dim, Pos pos, uint8_t color);
    void      colorize_region(Dim dim, Pos pos, uint8_t color);

    void low_colorize_1bit(Dim dim, Pos pos, uint8_t color);
    void low_colorize_3bit(Dim dim, Pos pos, uint8_t color);

    inline void clear()  {
      if (pixel_resolution == PixelResolution::ONE_BIT) {
        frame_buffer_1bit->clear();
      }
      else {
        frame_buffer_3bit->clear();
      }
    }

    inline void update(UpdateMode mode) {
      if (pixel_resolution == PixelResolution::ONE_BIT) {
        switch (mode) {
          case UpdateMode::FULL:
            e_ink.update(*frame_buffer_1bit);
            partial_count = PARTIAL_COUNT_ALLOWED;
            break;
          case UpdateMode::FORCE_PARTIAL:
          case UpdateMode::FAST:
            // No MODE_DU on Inkplate; FAST falls back to a partial update.
            e_ink.partial_update(*frame_buffer_1bit);
            partial_count = 0;
            break;
          case UpdateMode::BUDGETED:
          default:
            if (partial_count <= 0) {
              //e_ink.clean();
              e_ink.update(*frame_buffer_1bit);
              partial_count = PARTIAL_COUNT_ALLOWED;
            }
            else {
              e_ink.partial_update(*frame_buffer_1bit);
              partial_count--;
            }
            break;
        }
      }
      else {
        e_ink.update(*frame_buffer_3bit);
      }
    }

    inline void update(bool no_full = false) {
      update(no_full ? UpdateMode::FORCE_PARTIAL : UpdateMode::BUDGETED);
    }

  private:
    static constexpr char const * TAG = "Screen";
    static const uint8_t          LUT1BIT[8];
    static const uint8_t          LUT1BIT_INV[8];
    
    static uint16_t width;
    static uint16_t height;

    static Screen singleton;
    Screen() : partial_count(0), 
               frame_buffer_1bit(nullptr), 
               frame_buffer_3bit(nullptr) { };

    int16_t           partial_count;
    FrameBuffer1Bit * frame_buffer_1bit;
    FrameBuffer3Bit * frame_buffer_3bit;
    PixelResolution   pixel_resolution;
    Orientation       orientation;

    enum class Corner : uint8_t { TOP_LEFT, TOP_RIGHT, LOWER_LEFT, LOWER_RIGHT };
    void draw_arc(uint16_t x_mid,  uint16_t y_mid,  uint8_t radius, Corner corner, uint8_t color);

  public:
    static Screen & get_singleton() noexcept { return singleton; }
    /// `preserve_panel_image` is honored on PaperS3 only — Inkplate
    /// boards always run their normal init path. The parameter has a
    /// default so existing call sites (orientation change, etc.) need
    /// no edits.
    void setup(PixelResolution resolution, Orientation orientation,
               bool preserve_panel_image = false);
    void set_pixel_resolution(PixelResolution resolution, bool force = false);
    void set_orientation(Orientation orient);
    inline Orientation get_orientation() { return orientation; }
    inline PixelResolution get_pixel_resolution() { return pixel_resolution; }
    inline void force_full_update() { partial_count = 0; }

    #if INKPLATE_6PLUS
      void to_user_coord(uint16_t & x, uint16_t & y);
    #endif

    inline static uint16_t get_width() { return width; }
    inline static uint16_t get_height() { return height; }
};

#endif // BOARD_TYPE_PAPER_S3

#if __SCREEN__
  Screen & screen = Screen::get_singleton();
#else
  extern Screen & screen;
#endif
