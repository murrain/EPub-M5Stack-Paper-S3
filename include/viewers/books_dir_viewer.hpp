// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#pragma once
#include "global.hpp"

#include "models/books_dir.hpp"
#include "viewers/page.hpp"

class BooksDirViewer
{
  public:
    // Top region reserved for the persistent menu strip on touch
    // builds (Inkplate-6plus / TOUCH_TRIAL / PaperS3). The menu
    // is always shown as a header on the books-dir screen, so
    // both viewers shift book layout below this height. Button-
    // only builds keep the menu modal (no permanent strip), so
    // the reservation is zero. Keep this in lockstep with
    // Gestures::TOP_EDGE_PX — they describe the same geometry
    // (the menu strip's footprint and tap-hit region).
    #if (INKPLATE_6PLUS || TOUCH_TRIAL)
      static constexpr int16_t HEADER_RESERVED_HEIGHT = 80;
    #else
      static constexpr int16_t HEADER_RESERVED_HEIGHT = 0;
    #endif

    virtual void                        setup() = 0;

    virtual int16_t   show_page_and_highlight(int16_t book_idx) = 0;
    virtual void               highlight_book(int16_t book_idx) = 0;
    virtual void              clear_highlight() = 0;

    virtual int16_t    next_page() = 0;
    virtual int16_t    prev_page() = 0;
    virtual int16_t    next_item() = 0;
    virtual int16_t    prev_item() = 0;
    virtual int16_t  next_column() = 0;
    virtual int16_t  prev_column() = 0;

    virtual int16_t get_index_at(uint16_t x, uint16_t y) = 0;
};
