// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#define __BOOKS_DIR_CONTROLLER__ 1
#include "controllers/books_dir_controller.hpp"

#include "controllers/app_controller.hpp"
#include "controllers/book_controller.hpp"
#include "controllers/option_controller.hpp"
#include "viewers/menu_viewer.hpp"
#include "models/books_dir.hpp"
#include "models/config.hpp"
#include "models/epub.hpp"
#include "models/nvs_mgr.hpp"
#include "models/page_cache.hpp"
#include "models/page_locs.hpp"
#include "models/session_state.hpp"
#include "viewers/book_viewer.hpp"
#include "viewers/linear_books_dir_viewer.hpp"
#include "viewers/matrix_books_dir_viewer.hpp"
#include "viewers/msg_viewer.hpp"
#include "screen.hpp"

#include <cassert>

#if EPUB_INKPLATE_BUILD
  #include "models/nvs_mgr.hpp"
  #include "esp.hpp"
#endif

void
BooksDirController::setup()
{
  // Retrieve the information related to the last book read by the user. 
  // This is stored in the NVS on the ESP32, or in a flat file on Linux.
  // If the user was reading a book at the last entry to deep sleep, it will be
  // shown on screen instead of the books directory list.

  current_book_index         = -1;
  last_read_book_index       = -1;
  book_page_id.itemref_index = -1;
  book_page_id.offset        = -1;
  book_was_shown             = false;
  refresh_deferred           = false;


  #if EPUB_INKPLATE_BUILD

    int16_t dummy;
    // On warm wake, defer the SD-side directory refresh — the user
    // is going straight back to their book and the cached DB from
    // the previous session is accurate. The refresh fires lazily on
    // first navigation back to the books directory (see enter()).
    // On cold boot read_books_directory does the full refresh inline,
    // so a stale cache cannot accumulate across power cycles — every
    // cold boot is a natural catch-up point.
    const bool skip_refresh = SessionState::is_warm_wake();
    refresh_deferred = skip_refresh;
    if (skip_refresh) {
      LOG_I("Warm wake: deferring books-dir refresh until first dir-enter");
    }

    if (!books_dir.read_books_directory(nullptr, dummy, skip_refresh)) {
      LOG_E("There was issues reading books directory.");
    }
    else {

      NVSMgr::NVSData nvs_data;
      uint32_t        id;

      if (nvs_mgr.get_last(id, nvs_data)) {
        book_page_id.itemref_index = nvs_data.itemref_index;
        book_page_id.offset        = nvs_data.offset;
        book_was_shown             = nvs_data.was_shown;

        int16_t idx;
        if ((idx = books_dir.get_sorted_idx_from_id(id)) != -1) {

          last_read_book_index = current_book_index = idx;
          
          //LOG_D("Last book filename: %s",  book_fname);
          LOG_D("Last book ref index: %d", book_page_id.itemref_index);
          LOG_D("Last book offset: %d",    book_page_id.offset);
          LOG_D("Show it now: %s",         book_was_shown ? "yes" : "no");
        }
      }
    }

  #else

    char * book_fname          = new char[256];
    char * filename            = nullptr;

    book_fname[0]              =  0;

    FILE * f = fopen(MAIN_FOLDER "/last_book.txt", "r");
    filename = nullptr;
    if (f != nullptr) {

      if (fgets(book_fname, 256, f)) {
        int16_t size = strlen(book_fname) - 1;
        if (book_fname[size] == '\n') book_fname[size] = 0;

        char buffer[20];
        if (fgets(buffer, 20, f)) {
          book_page_id.itemref_index = atoi(buffer);

          if (fgets(buffer, 20, f)) {
            book_page_id.offset = atoi(buffer);

            if (fgets(buffer, 20, f)) {
              int8_t was_shown = atoi(buffer);
              filename       = book_fname;
              book_was_shown = (bool) was_shown;
            }
          }
        }
      }

      fclose(f);
    } 

    int16_t db_idx = -1;
    // Read the directory, returning the book index (db_idx).
    if (!books_dir.read_books_directory(filename, db_idx)) {
      LOG_E("There was issues reading books directory.");
    }
    
    // The retrieved db_idx is the index in the database of the last book
    // read by the user. We need the
    // index in the sorted list of books as this is what the 
    // BookController expect.

    if (db_idx != -1) {
      last_read_book_index = books_dir.get_sorted_idx(db_idx);
      current_book_index   = last_read_book_index;
      book_filename        = book_fname;
    }

    LOG_D("Book to show: idx:%d page:(%d, %d) was_shown:%s", 
          last_read_book_index, book_page_id.itemref_index, book_page_id.offset, book_was_shown ? "yes" : "no");

    delete [] book_fname;
  #endif
}

void
BooksDirController::save_last_book(const PageLocs::PageId & page_id, bool going_to_deep_sleep)
{
  // As we leave, we keep the information required to return to the book
  // in the NVS space. If this is called just before going to deep sleep, we
  // set the "WAS_SHOWN" boolean to true, such that when the device will
  // be booting, it will display the last book at the last page shown.

  book_page_id = page_id;

  #if EPUB_INKPLATE_BUILD

    uint32_t book_id;

    if ((current_book_index != -1) && books_dir.get_book_id(current_book_index, book_id)) {

      NVSMgr::NVSData nvs_data = {
        .offset        = page_id.offset,
        .itemref_index = page_id.itemref_index,
        .was_shown     = (uint8_t) (going_to_deep_sleep ? 1 : 0),
        .filler1       = 0
      };

      if (!nvs_mgr.save_location(book_id, nvs_data)) {
        LOG_E("Unable to save current ebook location");
      }
      last_read_book_index = 
      current_book_index   = books_dir.get_sorted_idx_from_id(book_id);
    }

  #else
  
    FILE * f = fopen(MAIN_FOLDER "/last_book.txt", "w");
    if (f != nullptr) {
      fprintf(f, "%s\n%d\n%d\n%d\n",
        book_filename.c_str(),
        page_id.itemref_index,
        page_id.offset,
        going_to_deep_sleep ? 1 : 0
      );
      fclose(f);
    } 
  #endif  
}

void
BooksDirController::show_last_book()
{

  if (last_read_book_index == -1) return;

  LOG_D("===> show_last_book()...");
  static std::string            book_fname;
  static std::string            book_title;
  const BooksDir::EBookRecord * book;

  book_was_shown = false;  
  book           = books_dir.get_book_data(last_read_book_index);

  if (book != nullptr) {
    book_fname  = BOOKS_FOLDER "/";
    book_fname += book->filename;
    book_title  = book->title;
    if (book_controller.open_book_file(book_title, book_fname, book_page_id)) {
      LOG_W("show_last_book: phase: set_controller(BOOK)");
      app_controller.set_controller(AppController::Ctrl::BOOK);
      LOG_W("show_last_book: phase: set_controller_returned");
    }
    else {
      // Open failed (most commonly: page_locs.retrieve_asap timed out
      // because the retriever is wedged on a malformed embedded font
      // or runaway layout). The runtime member book_was_shown is
      // already false (set at line ~203), so this enter() won't loop;
      // but NVS still records was_shown=1 from the previous deep
      // sleep, which would re-trap the user on the very next cold
      // boot. Persist was_shown=0 immediately so a power cycle lands
      // them in the books-dir UI instead of replaying the wedge.
      //
      // The user can still tap the offending book to retry — that's
      // a deliberate action and they'll see the same diagnostic msg
      // and a fast bail. This is the loop-breaker, not a permanent
      // skip.
      save_last_book(book_page_id, false);

      msg_viewer.show(
        MsgViewer::MsgType::ALERT,
        false, true,
        "Could not open book",
        "\"%s\" could not be opened. Returning to the library. "
        "Pick another book to continue.",
        book_title.c_str());
    }
  }
}

void
BooksDirController::enter()
{

  LOG_D("===> enter()...");

  // Tear down any previously-active book before rendering the dir.
  // BooksDirController is the natural close point for a book session
  // — symmetric with how books get OPENED from here via show_last_book
  // and the user-tap handlers that call book_controller.open_book_file.
  // Putting the close here (rather than in BookController::leave)
  // avoids breaking BOOK→PARAM and BOOK→TOC transitions, both of which
  // run leave() but expect the file to remain open.
  //
  // Safe on first cold-boot enter: file_is_open is false, close_file
  // short-circuits. Safe on warm-wake auto-resume (book_was_shown
  // path below): close_file no-ops, then show_last_book → open_book_
  // file → epub.open_file does its own internal close-then-open
  // anyway.
  //
  // Lock-order discipline: pre-paint stop FIRST, then page_locs
  // stop, then epub close. Pre-paint task acquires book_viewer.
  // get_mutex during render and dereferences PageLocs::item_info
  // + EPub state — freeing those before pre-paint is confirmed
  // idle would UAF mid-render. STOP/STOPPED handshake inside
  // page_cache.stop guarantees the task has exited any
  // ScopedRenderTarget and released book_viewer's mutex before
  // returning. See architecture review M4 for the rationale.
  //
  // page_locs.stop_document() must precede close_file so the
  // RetrieverTask isn't mid-build when EPub state goes away (which
  // would page-fault on item_info.xml_doc dereferences). The new
  // stop_document also deep-cleans PageLocs::item_info, which would
  // otherwise hold dangling css_list pointers into the now-freed
  // EPub::css_cache.
  page_cache.stop();
  page_locs.stop_document();
  epub.close_file();

  config.get(Config::Ident::DIR_VIEW, &viewer_id);
  const char *view = (viewer_id == LINEAR_VIEWER) ? "linear" :
                     (viewer_id == MATRIX_VIEWER) ? "matrix" : "unknown";
  log('I', TAG,
      "BooksDirController enter: viewer=%s (id=%d) screen=%dx%d",
      view, viewer_id, Screen::get_width(), Screen::get_height());
  books_dir_viewer = (viewer_id == LINEAR_VIEWER) ? (BooksDirViewer *) &linear_books_dir_viewer :
                                        (BooksDirViewer *) &matrix_books_dir_viewer;

  books_dir_viewer->setup();
  screen.force_full_update();

  if (book_was_shown && (last_read_book_index != -1)) {
    // Warm-wake auto-resume path: jump straight to the user's
    // book. Do NOT flush the deferred refresh here — that would
    // run the SD-side directory scan synchronously on the resume
    // path and defeat the entire point of the deferral. The
    // refresh fires on the FIRST manual navigation back to the
    // directory (the else branch below) which is the natural
    // moment we owe the user a fresh listing.
    show_last_book();
  }
  else {
    // The user is actually looking at the directory. Flush any
    // deferred refresh from a warm-wake setup() before drawing
    // the page so the listing is up to date.
    if (refresh_deferred) {
      LOG_I("Flushing deferred books-dir refresh");
      int16_t dummy;
      if (!books_dir.refresh(nullptr, dummy)) {
        LOG_E("Deferred books-dir refresh failed");
      }
      refresh_deferred = false;
    }

    if (current_book_index == -1) current_book_index = 0;
    current_book_index = books_dir_viewer->show_page_and_highlight(current_book_index);
  }

  #if (INKPLATE_6PLUS || TOUCH_TRIAL)
    // Paint the option menu as a persistent header strip on top
    // of the just-rendered books area. The books viewer's
    // first_entry_ypos was sized using BooksDirViewer::
    // get_header_height() so the strip sits in unrendered
    // (white) space — no overlap with book covers or list rows.
    // Strip taps are dispatched inline by input_event below
    // (no transition to a separate OPTION controller state).
    // menu_viewer.show() commits via partial update_region for
    // the strip area only — the just-painted books area stays
    // put on the panel.
    option_controller.show_menu();

    // Drift assert: get_header_height() (the static formula
    // mirror) and get_region_height() (the value computed
    // inside show()) must agree. If they ever disagree, the
    // formula in MenuViewer::compute_region_height has drifted
    // from MenuViewer::show — fix the formula, not the assert.
    // Both run after fonts load, so this is a tight equality.
    assert(menu_viewer.get_region_height() == BooksDirViewer::get_header_height());
  #endif
}

void
BooksDirController::leave(bool going_to_deep_sleep)
{

}

void
BooksDirController::refresh_view()
{
  // Refresh-view is the in-place redraw entry point. It deliberately
  // skips the book-session teardown that enter() does (page_cache.
  // stop / page_locs.stop_document / epub.close_file), because by
  // construction no book session is open: the only way to open a
  // book transitions OUT of DIR (show_last_book / tap-to-open both
  // call set_controller(BOOK)). If a future caller invokes this
  // from a state where a book IS open, the missing teardown would
  // leak resources and race the live book session against the
  // refresh's repaint. Catch that misuse early.
  assert(!epub.is_file_open());

  // The books_dir database may have grown or shrunk during the
  // refresh. Clamp current_book_index to a valid range before
  // re-rendering — show_page_and_highlight will further clamp
  // internally but we want to give it a sensible starting point.
  // Note: an empty post-refresh library (book_count == 0)
  // propagates to show_page_and_highlight unchecked, same as
  // enter() at line 291. If that ever surfaces as a bug, both
  // sites need the same guard. Pre-existing condition.
  if (current_book_index < 0 ||
      current_book_index >= books_dir.get_book_count()) {
    current_book_index = 0;
  }

  // Re-derive the viewer pointer in case dir_view changed since
  // last enter() (matches enter()'s pattern). show_page_and_-
  // highlight then redraws the books area — using paint_region
  // under the hood, so the persistent strip's framebuffer +
  // panel pixels are not touched.
  //
  // No screen.force_full_update() here (unlike enter()):
  // msg_viewer's "Refreshing..." banner was painted via a
  // normal BUDGETED update, so a region repaint cleanly
  // overwrites it. Forcing a full GC16 would be a wasted
  // ~700 ms refresh with no ghost-cleanup benefit.
  config.get(Config::Ident::DIR_VIEW, &viewer_id);
  books_dir_viewer = (viewer_id == LINEAR_VIEWER)
                       ? (BooksDirViewer *) &linear_books_dir_viewer
                       : (BooksDirViewer *) &matrix_books_dir_viewer;
  books_dir_viewer->setup();
  current_book_index = books_dir_viewer->show_page_and_highlight(current_book_index);

  #if (INKPLATE_6PLUS || TOUCH_TRIAL)
    // Strip was overwritten by msg_viewer.show inside refresh —
    // restore. show_page_and_highlight's paint_region only
    // touches the books area, but msg_viewer's earlier full-
    // screen paint clobbered the strip's framebuffer pixels and
    // panel pixels both, so the strip needs a fresh paint here.
    option_controller.show_menu();
  #endif
}

#if INKPLATE_6PLUS || TOUCH_TRIAL
  void
  BooksDirController::input_event(const EventMgr::Event & event)
  {
    static std::string book_fname;
    static std::string book_title;

    const BooksDir::EBookRecord * book;

    // Sub-state takes priority. When the user picked a menu icon
    // that opened a sub-form (main params, font params, date/time,
    // calibration) or a key-press handshake (post-wifi, post-usb),
    // the form_viewer / msg_viewer covers the screen and owns
    // input. Delegate to OptionController's existing dispatch —
    // it knows how to drive each sub-state and clear its flag on
    // completion. After the dispatch we check whether the sub-
    // state ended on this event; if so, the form left a stale
    // image on the framebuffer where books used to be, so re-
    // render the books area + strip to restore the books-dir
    // screen.
    if (option_controller.has_active_sub_state()) {
      // skip_strip_refresh=true: we re-render below after the
      // sub-state ends; the in-dispatch menu_viewer.show calls
      // would commit a partial EPD update that the outer render
      // immediately overwrites. See MenuControllerBase header.
      option_controller.dispatch_to_sub_state(event, /*skip_strip_refresh=*/true);
      if (!option_controller.has_active_sub_state()) {
        // Sub-state just ended. Restore the books-dir UI.
        books_dir_viewer->show_page_and_highlight(current_book_index);
        option_controller.show_menu();
      }
      return;
    }

    // Tap on the persistent menu strip (top get_header_height()
    // pixels) → fire the icon's action callback inline. Action
    // may set a sub-state flag on OptionController; the next
    // event routes to the dispatch above. The strip area never
    // holds books (the books viewers reserve this space at
    // setup() time using the same get_header_height value), so
    // a tap here is unambiguously menu, never a book selection.
    if (event.kind == EventMgr::EventKind::TAP &&
        event.y < BooksDirViewer::get_header_height()) {
      menu_viewer.event(event);
      // For direct actions (refresh books, return to last book)
      // the action transitions out of DIR via set_controller, so
      // any lingering icon highlight is cleared on the next DIR
      // entry. For sub-form-launching actions the form covers the
      // strip immediately and clear_highlight runs when the form
      // completes (inside dispatch_to_sub_state). No extra clear
      // needed here.
      return;
    }

    switch (event.kind) {
      case EventMgr::EventKind::SWIPE_RIGHT:
        // No strip restore needed — show_page now uses
        // page.paint_region to clear + commit only the books
        // area, leaving the strip's framebuffer + panel pixels
        // untouched. The earlier show_menu() call here was a
        // workaround for the strip being wiped by show_page's
        // full-screen page.paint(); that's no longer the case.
        current_book_index = books_dir_viewer->prev_page();
        break;

      case EventMgr::EventKind::SWIPE_LEFT:
        current_book_index = books_dir_viewer->next_page();
        break;

      case EventMgr::EventKind::TAP:
        // Universal hit-test on the books area (y >=
        // get_header_height(), after the early return above).
        // LinearBooksDirViewer::get_index_at hits the full row
        // width — see include/viewers/linear_books_dir_viewer
        // .hpp::get_index_at — so a tap on the title or author
        // text selects the same book as a tap on the cover.
        current_book_index = books_dir_viewer->get_index_at(event.x, event.y);
        if ((current_book_index >= 0) && (current_book_index < books_dir.get_book_count())) {
          book = books_dir.get_book_data(current_book_index);
          if (book != nullptr) {
            last_read_book_index = current_book_index;
            book_fname    = BOOKS_FOLDER "/";
            book_fname   += book->filename;
            book_title    = book->title;
            book_filename = book->filename;

            PageLocs::PageId page_id = { 0, 0 };

            #if EPUB_INKPLATE_BUILD
              NVSMgr::NVSData nvs_data;
              if (nvs_mgr.get_location(book->id, nvs_data)) {
                page_id = { nvs_data.itemref_index, nvs_data.offset };
              }
            #endif

            if (book_controller.open_book_file(book_title, book_fname, page_id)) {
              LOG_W("input_event_TAP: phase: set_controller(BOOK)");
              app_controller.set_controller(AppController::Ctrl::BOOK);
              LOG_W("input_event_TAP: phase: set_controller_returned");
            }
          }
        }
        // No fallback to a separate OPTION state — a tap below
        // the last row in linear view, or on a partial-page edge
        // in matrix view, is treated as a no-op. The persistent
        // menu strip is always available for menu actions.
        break;

      case EventMgr::EventKind::HOLD:
        current_book_index = books_dir_viewer->get_index_at(event.x, event.y);
        if ((current_book_index >= 0) && (current_book_index < books_dir.get_book_count())) {
          books_dir_viewer->highlight_book(current_book_index);
          LOG_I("Book Index: %d", current_book_index);
        }
        break;

      case EventMgr::EventKind::RELEASE:
        #if INKPLATE_6PLUS
          ESP::delay(1000);
        #endif
        
        books_dir_viewer->clear_highlight();
        break;

      default:
        break;
    }
  }
#else
  void 
  BooksDirController::input_event(const EventMgr::Event & event)
  {
    static std::string book_fname;
    static std::string book_title;

    const BooksDir::EBookRecord * book;

    switch (event.kind) {
      #if EXTENDED_CASE
        case EventMgr::EventKind::PREV:
      #else
        case EventMgr::EventKind::DBL_PREV:
      #endif
        current_book_index = books_dir_viewer->prev_column();   
        break;

      #if EXTENDED_CASE
        case EventMgr::EventKind::NEXT:
      #else
        case EventMgr::EventKind::DBL_NEXT:
      #endif
        current_book_index = books_dir_viewer->next_column();
        break;

      #if EXTENDED_CASE
        case EventMgr::EventKind::DBL_PREV:
      #else
        case EventMgr::EventKind::PREV:
      #endif
        current_book_index = books_dir_viewer->prev_item();
        break;

      #if EXTENDED_CASE
        case EventMgr::EventKind::DBL_NEXT:
      #else
        case EventMgr::EventKind::NEXT:
      #endif
        current_book_index = books_dir_viewer->next_item();
        break;

      case EventMgr::EventKind::SELECT:
        if (current_book_index < books_dir.get_book_count()) {
          book = books_dir.get_book_data(current_book_index);
          if (book != nullptr) {
            last_read_book_index = current_book_index;
            book_fname    = BOOKS_FOLDER "/";
            book_fname   += book->filename;
            book_title    = book->title;
            book_filename = book->filename;
            
            PageLocs::PageId page_id = { 0, 0 };

            #if EPUB_INKPLATE_BUILD
              NVSMgr::NVSData nvs_data;
              if (nvs_mgr.get_location(book->id, nvs_data)) {
                page_id.itemref_index = nvs_data.itemref_index;
                page_id.offset        = nvs_data.offset;
              }
            #endif

            if (book_controller.open_book_file(book_title, book_fname, page_id)) {
              app_controller.set_controller(AppController::Ctrl::BOOK);
            }
          }
        }
        break;

      case EventMgr::EventKind::DBL_SELECT:
        app_controller.set_controller(AppController::Ctrl::OPTION);
        break;
        
      case EventMgr::EventKind::NONE:
        break;
    }
  }
#endif