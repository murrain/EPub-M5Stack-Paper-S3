// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#define __BOOKS_DIR_CONTROLLER__ 1
#include "controllers/books_dir_controller.hpp"

#include "controllers/app_controller.hpp"
#include "controllers/book_controller.hpp"
#include "controllers/gestures.hpp"
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
#include "screen.hpp"

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
      app_controller.set_controller(AppController::Ctrl::BOOK);
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
}

void 
BooksDirController::leave(bool going_to_deep_sleep)
{

}

#if INKPLATE_6PLUS || TOUCH_TRIAL
  void
  BooksDirController::input_event(const EventMgr::Event & event)
  {
    static std::string book_fname;
    static std::string book_title;

    const BooksDir::EBookRecord * book;

    // SWIPE_DOWN-from-top opens the menu (the unambiguous "drawer
    // pull" gesture). NOT is_menu_open — that variant also fires
    // on TAP-at-top, which steals the first-row book selection
    // in matrix view (the first cover sits at y<TOP_EDGE_PX) and
    // the top-of-list cell in linear view. The TAP case below
    // already does book-hit-first with an OPTION fallback when
    // no book is hit at the tap location, so a tap-at-top with
    // no book under it still opens the menu — just via the
    // hit-test path instead of an early return.
    if (Gestures::is_menu_open_swipe(event)) {
      current_book_index = -1;
      app_controller.set_controller(AppController::Ctrl::OPTION);
      return;
    }

    switch (event.kind) {
      case EventMgr::EventKind::SWIPE_RIGHT:
        current_book_index = books_dir_viewer->prev_page();
        break;

      case EventMgr::EventKind::SWIPE_LEFT:
        current_book_index = books_dir_viewer->next_page();
        break;

      case EventMgr::EventKind::TAP:
        // Universal hit-test: a tap that lands on a book selects
        // it, regardless of which view is active. LinearBooksDir-
        // Viewer::get_index_at hits the full row width (no x-
        // filter — see its impl), so a tap on the title or author
        // text selects the same book as a tap on the cover. Matrix
        // view already worked this way. The legacy "tap on right
        // two-thirds in linear view → menu" path is gone — menu
        // open is the SWIPE_DOWN-from-top gesture, plus the
        // fall-through here when get_index_at returns no hit
        // (tap below the last row, or on a partial-page edge).
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
              app_controller.set_controller(AppController::Ctrl::BOOK);
            }
          }
        }
        else {
          current_book_index = -1;
          app_controller.set_controller(AppController::Ctrl::OPTION);
        }
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