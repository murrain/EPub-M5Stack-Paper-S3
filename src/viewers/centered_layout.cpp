// Copyright (c) 2026 EPub-InkPlate contributors
//
// MIT License. Look at file licenses.txt for details.

#include "viewers/centered_layout.hpp"

#if EPUB_INKPLATE_BUILD && defined(BOARD_TYPE_PAPER_S3)

#include "screen.hpp"

namespace CenteredLayout {

Page::Format make_fmt(int8_t           font_index,
                      int16_t          font_size,
                      Fonts::FaceStyle style,
                      CSS::Align       align,
                      double           line_height_factor)
{
  Page::Format fmt = {
    .line_height_factor = line_height_factor,
    .font_index         = font_index,
    .font_size          = font_size,
    .indent             = 0,
    .margin_left        = 0,
    .margin_right       = 0,
    .margin_top         = 0,
    .margin_bottom      = 0,
    .screen_left        = PAD,
    .screen_right       = PAD,
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

void put_centered(const std::string & text, int16_t y, Page::Format & fmt)
{
  fmt.align = CSS::Align::CENTER;
  page.put_str_at(text, Pos(Page::HORIZONTAL_CENTER, y), fmt);
}

void draw_hrule(int16_t W, int16_t y, int16_t inset, int16_t thickness)
{
  int16_t left  = PAD + inset;
  int16_t width = W - 2 * (PAD + inset);
  if (width <= 0) return;
  screen.colorize_region(Dim((uint16_t) width, (uint16_t) thickness),
                         Pos(left, y), Screen::BLACK_COLOR);
}

} // namespace CenteredLayout

#endif // EPUB_INKPLATE_BUILD && BOARD_TYPE_PAPER_S3
