// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#pragma once
#include "global.hpp"

#include "controllers/event_mgr.hpp"
#include "models/epub.hpp"
#include "models/page_locs.hpp"
#include "viewers/books_dir_viewer.hpp"

class BooksDirController
{
  private:
    static constexpr char const * TAG = "BooksDirController";

    const uint8_t LINEAR_VIEWER = 0;
    const uint8_t MATRIX_VIEWER = 1;

    int32_t     book_offset;
    int16_t     current_book_index;
    int16_t     last_read_book_index;
    std::string book_filename;
    bool        book_was_shown;

    // True when setup() took the warm-wake fast path and skipped the
    // BooksDir refresh. Cleared the first time enter() runs (which is
    // when the user navigates to the books directory and we owe them
    // a fresh listing). See BooksDirController::enter for the flush.
    // Initialized at declaration so the invariant holds even if a
    // future code path reaches enter() without going through setup().
    bool        refresh_deferred = false;

    PageLocs::PageId book_page_id;
    BooksDirViewer * books_dir_viewer;
    int8_t viewer_id;

  public:
    BooksDirController() {};
    void setup();
    void input_event(const EventMgr::Event & event);
    void enter();
    void leave(bool going_to_deep_sleep = false);
    void save_last_book(const PageLocs::PageId & page_id, bool going_to_deep_sleep);
    void show_last_book();
    void new_orientation() { if (books_dir_viewer != nullptr) books_dir_viewer->setup(); }

    // Re-render the books-dir UI in place, without going through
    // a full controller transition (no leave/enter, no book
    // teardown — we never left the books-dir screen). Used after
    // CommonActions::refresh_books_dir on touch builds where the
    // persistent strip means the action was dispatched from
    // inside BooksDirController::input_event itself; the usual
    // OPTION→DIR transition that would re-render via enter()
    // doesn't happen because we never went OPTION in the first
    // place. Without this, the msg_viewer "Refreshing..." banner
    // stays on screen forever — looks like a hang.
    void refresh_view();

    inline int16_t get_current_book_index() { return current_book_index; }
    inline void    set_current_book_index(int16_t idx) { current_book_index = idx; }
};

#if __BOOKS_DIR_CONTROLLER__
  BooksDirController books_dir_controller;
#else
  extern BooksDirController books_dir_controller;
#endif
