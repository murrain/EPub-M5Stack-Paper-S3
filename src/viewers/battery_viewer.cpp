// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#define __BATTERY_VIEWER__ 1
#include "viewers/battery_viewer.hpp"

#if EPUB_INKPLATE_BUILD
  #include "viewers/page.hpp"
  #include "models/config.hpp"
  #if defined(BOARD_TYPE_PAPER_S3)
    #include "battery_paper_s3.hpp"
  #else
    #include "battery.hpp"
  #endif
  #include "screen.hpp"
  #include "logging.hpp"

  #include <cstring>

  static constexpr char const * TAG = "BatteryViewer";

  void
  BatteryViewer::show()
  {
    int8_t view_mode;
    config.get(Config::Ident::BATTERY, &view_mode);

    if (view_mode == 0) return;

    float voltage = battery.read_level();

    LOG_D("Battery voltage: %5.3f", voltage);

    Page::Format fmt = {
      .line_height_factor = 1.0,
      .font_index         = 1,
      .font_size          = 9,
      .indent             = 0,
      .margin_left        = 0,
      .margin_right       = 0,
      .margin_top         = 0,
      .margin_bottom      = 0,
      .screen_left        = 10,
      .screen_right       = 10,
      .screen_top         = 10,
      .screen_bottom      = 10,
      .width              = 0,
      .height             = 0,
      .vertical_align     = 0,
      .trim               = true,
      .pre                = false,
      .font_style         = Fonts::FaceStyle::NORMAL,
      .align              = CSS::Align::LEFT,
      .text_transform     = CSS::TextTransform::NONE,
      .display            = CSS::Display::INLINE
    };

    // Show battery icon

    Font * font = fonts.get(0);

    if (font == nullptr) {
      LOG_E("Internal error (Drawings Font not available!");
      return;
    }

    // Map voltage to icon index 0..4.
    // Inkplate uses a 2x AAA pack (2.5V empty .. 3.7V full nominal); the
    // PaperS3 uses a single Li-ion cell (3.3V empty .. 4.15V full).
    #if defined(BOARD_TYPE_PAPER_S3)
      float   value = ((voltage - 3.30) * 4.0) / 0.85;
    #else
      float   value = ((voltage - 2.5) * 4.0) / 1.2;
    #endif
    int16_t icon_index = value;
    if (icon_index < 0) icon_index = 0;
    if (icon_index > 4) icon_index = 4;

    static constexpr char icons[5] = { '0', '1', '2', '3', '4' };

    Font::Glyph * glyph = font->get_glyph(icons[icon_index], 9);

    Dim dim;
    dim.width  =  100;
    dim.height = -font->get_descender_height(9);

    Pos pos;
    // pos.x = 4;
    pos.y = Screen::get_height() + font->get_descender_height(9) - 2;

    // page.clear_region(dim, pos);

    fmt.font_index = 0;  
    pos.x          = 5;
    page.put_char_at(icons[icon_index], pos, fmt);

    // LOG_E("Battery icon index: %d (%c)", icon_index, icons[icon_index]);

    // Show text

    if ((view_mode == 1) || (view_mode == 2)) {
      char str[10];

      if (view_mode == 1) {
        #if defined(BOARD_TYPE_PAPER_S3)
          int percentage = battery.read_percentage();
        #else
          int percentage = ((voltage - 2.5) * 100.0) / 1.2;
        #endif
        if (percentage > 100) percentage = 100;
        if (percentage < 0)   percentage = 0;
        sprintf(str, "%d%c", percentage, '%');
      }
      else if (view_mode == 2) {
        sprintf(str, "%5.2fv", voltage);
      }

      font = fonts.get(1);
      fmt.font_index = 1;  
      pos.x = 5 + (glyph != nullptr ? glyph->advance : 10) + 5;
      page.put_str_at(str, pos, fmt);
    }
  }
#endif