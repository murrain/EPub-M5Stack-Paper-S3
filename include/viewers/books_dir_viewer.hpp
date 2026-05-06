// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#pragma once
#include "global.hpp"

#include "models/books_dir.hpp"
#include "viewers/page.hpp"
#include "viewers/menu_viewer.hpp"

class BooksDirViewer
{
  public:
    // Top region reserved for the persistent menu strip on touch
    // builds (Inkplate-6plus / TOUCH_TRIAL / PaperS3). Computed
    // dynamically from MenuViewer::compute_region_height() so the
    // strip's rendered height (which depends on font metrics
    // for the icon glyph and caption line) is always honored.
    //
    // Why dynamic: an earlier static `constexpr int16_t ... = 80`
    // bootlooped on PaperS3 because the drawings.otf icon font
    // at ICON_SIZE=15 plus the caption line at CAPTION_SIZE=12
    // rendered larger than 80 px, tripping the layout-overflow
    // assert on every BooksDirController::enter. The fix isn't
    // to pick a bigger magic number — it's to query the source
    // of truth.
    //
    // Button-only builds have no persistent strip and return 0;
    // their menu is the modal OPTION controller transition.
    //
    // Caller must ensure fonts are loaded before invoking; safe
    // from any controller's setup()/enter() since AppController
    // initializes fonts before dispatching to controllers.
    static inline uint16_t get_header_height() {
      #if (INKPLATE_6PLUS || TOUCH_TRIAL)
        return MenuViewer::compute_region_height();
      #else
        return 0;
      #endif
    }

    // Helper for the strip-aware paint region: the rectangle
    // OUTSIDE which the persistent menu strip lives. Used by
    // both viewers' show_page when calling Page::paint_region —
    // single source of truth so a future tweak to the geometry
    // (e.g. a side gutter for a thumb-rest area) lands in one
    // place. Position + dimension form the books area:
    //   Pos(0, header), Dim(screen_width, screen_height - header)
    static inline void get_books_region(Pos & pos, Dim & dim) {
      const uint16_t header = get_header_height();
      pos = Pos(0, header);
      dim = Dim(Screen::get_width(), Screen::get_height() - header);
    }

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
