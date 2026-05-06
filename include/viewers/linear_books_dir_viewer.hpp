// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#pragma once
#include "global.hpp"

#include "models/books_dir.hpp"
#include "viewers/page.hpp"
#include "viewers/books_dir_viewer.hpp"

class LinearBooksDirViewer : public BooksDirViewer
{
  private:
    static constexpr char const * TAG = "LinearBooksDirView";

    static const int16_t TITLE_FONT            =  1;
    static const int16_t AUTHOR_FONT           =  2;
    static const int16_t TITLE_FONT_SIZE       = 11;
    static const int16_t AUTHOR_FONT_SIZE      =  9;
    static const int16_t SPACE_BETWEEN_ENTRIES =  6;
    static const int16_t MAX_TITLE_SIZE        = 85;

    // Where the book list starts. On touch builds the persistent
    // menu strip occupies the top get_header_height() pixels and
    // we leave a small gap below; on button builds there's no
    // strip so this is just the original 5 px top margin.
    // Initialized in setup() because the touch value depends on
    // font metrics (see BooksDirViewer::get_header_height) and
    // can't be a static constexpr.
    int16_t first_entry_ypos;

    int16_t current_item_idx;
    int16_t current_book_idx;
    int16_t current_page_nbr;
    int16_t books_per_page;
    int16_t page_count;

    void  show_page(int16_t page_nbr, int16_t hightlight_item_idx);
    void  highlight(int16_t item_idx);

  public:

    LinearBooksDirViewer() : first_entry_ypos(0), current_item_idx(-1), current_page_nbr(-1) {}
    
    void setup();
    
    int16_t   show_page_and_highlight(int16_t book_idx);
    void               highlight_book(int16_t book_idx);
    void              clear_highlight() { }

    int16_t   next_page();
    int16_t   prev_page();
    int16_t   next_item();
    int16_t   prev_item();
    int16_t next_column();
    int16_t prev_column();

    int16_t get_index_at(uint16_t x, uint16_t y) {
      // Guard against unsigned underflow when the tap lands in
      // the dead zone at the very top of the screen. Without
      // this check (y - first_entry_ypos) wraps and divides
      // into a huge idx that happens to fail the books_per_page
      // bounds check — accidentally correct but relying on
      // overflow arithmetic. Mirrors the explicit y guard in
      // MatrixBooksDirViewer::get_index_at.
      if (y < first_entry_ypos) return -1;
      int16_t idx = (y - first_entry_ypos) / (BooksDir::max_cover_height + SPACE_BETWEEN_ENTRIES);
      return (idx >= books_per_page) ? -1 : (current_page_nbr * books_per_page) + idx;
    }
};

#if __LINEAR_BOOKS_DIR_VIEWER__
  LinearBooksDirViewer linear_books_dir_viewer;
#else
  extern LinearBooksDirViewer linear_books_dir_viewer;
#endif
