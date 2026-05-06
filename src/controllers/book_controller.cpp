// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#define __BOOK_CONTROLLER__ 1
#include "controllers/book_controller.hpp"

#include "controllers/app_controller.hpp"
#include "controllers/books_dir_controller.hpp"
#include "models/books_dir.hpp"
#include "models/epub.hpp"
#include "models/session_state.hpp"
#include "models/wake_snapshot.hpp"
#include "viewers/book_viewer.hpp"
#include "viewers/page.hpp"
#include "viewers/msg_viewer.hpp"

#if EPUB_INKPLATE_BUILD
  #include "nvs.h"
#endif

// Single rendering + snapshot-capture path. Every navigation handler
// below goes through here rather than calling book_viewer.show_page
// directly — that keeps BookViewer a pure renderer and ensures every
// painted page is also captured for the warm-wake fast path.
void
BookController::show_and_capture(const PageLocs::PageId & page_id)
{
  book_viewer.show_page(page_id);

  // Identify the book by its NVS id so the snapshot meta survives the
  // sorted-index reshuffle that read_books_directory does each session.
  // Skip the capture if we somehow paint without an active book — the
  // snapshot would have no meaningful book_id to invalidate against
  // and would just sit on disk shadowing the real (next) book's wake.
  int16_t  bidx = books_dir_controller.get_current_book_index();
  uint32_t book_id = 0;
  if ((bidx >= 0) && books_dir.get_book_id((uint16_t)bidx, book_id)) {
    uint32_t fh = WakeSnapshot::format_params_hash(
                    epub.get_book_format_params(),
                    sizeof(EPub::BookFormatParams));
    wake_snapshot.capture(book_id, page_id, fh);
  }
}

void
BookController::enter()
{
  LOG_D("===> Enter()...");

  page_locs.check_for_format_changes(epub.get_item_count(), current_page_id.itemref_index);
  const PageLocs::PageId * id = page_locs.get_page_id(current_page_id);
  if (id != nullptr) {
    current_page_id.itemref_index = id->itemref_index;
    current_page_id.offset        = id->offset;
  }
  else {
    current_page_id.itemref_index = 0;
    current_page_id.offset        = 0;
  }
  show_and_capture(current_page_id);
}

void
BookController::leave(bool going_to_deep_sleep)
{
  LOG_D("===> leave()...");

  books_dir_controller.save_last_book(current_page_id, going_to_deep_sleep);

  if (!going_to_deep_sleep) {
    // Regular navigation back to dir/options/etc. Tear down the
    // active book to free heap. Without this, the previous book's
    // OPF buffer, encryption buffer, EPub::css_cache, fonts loaded
    // for the book, and PageLocs::item_info's resident pugixml DOM
    // + CSS suite + raw data all stayed pinned until the user
    // happened to open another book — at which point epub.open_file
    // would call close_file as a side-effect. With sequential book
    // opens (read book A, back to dir, open book B, repeat) the
    // cleanup window kept narrowing as the next book's allocations
    // had to fit alongside the previous one's still-resident state,
    // eventually exhausting internal SRAM contig blocks.
    //
    // page_locs.stop_document() must come before close_file: the
    // RetrieverTask may be mid-build when the user hits "back",
    // and we need it idle before we yank the EPub state from
    // under it (otherwise it page-faults on item_info's freed
    // xml_doc on the next iteration).
    //
    // Skipped on going_to_deep_sleep=true because the SleepScreen
    // viewer that paints during the sleep-entry window needs fonts
    // alive (close_file calls fonts.clear). The PMU power-cut
    // releases everything anyway when sleep actually fires.
    page_locs.stop_document();
    epub.close_file();
  }
}

bool
BookController::open_book_file(
  const std::string & book_title, 
  const std::string & book_filename, 
  const PageLocs::PageId & page_id)
{
  LOG_D("===> open_book_file()...");

  // Skip the "Loading a book" splash on warm wake so the sleep-screen
  // / wallpaper stays visible until the first page is actually drawn.
  // Replacing a beautiful wallpaper with a generic loading message for
  // ~700 ms is the worst possible UX on resume.
  const bool warm = SessionState::is_warm_wake();
  if (!warm) {
    msg_viewer.show(MsgViewer::MsgType::BOOK, false, false, "Loading a book",
       "The book \" %s \" is loading. Please wait.", book_title.c_str());
  }
  // Consume the warm-wake budget here — subsequent book opens during
  // this session are user-driven navigation (back to dir, pick another
  // book) and deserve the normal splash UX.
  //
  // Cleared unconditionally (before the actual open work). If
  // open_file() fails for the resume target, the user is returned to
  // the books-dir and any retry there gets normal splash UX, which is
  // the right semantics — the resume *was* attempted, the wallpaper
  // was preserved as long as possible, and a failed resume is now a
  // fresh user-driven navigation.
  SessionState::clear_warm_wake();

  bool new_document = book_filename != epub.get_current_filename();

  if (new_document) page_locs.stop_document();

  if (epub.open_file(book_filename)) {
    if (new_document) {
      page_locs.start_new_document(epub.get_item_count(), page_id.itemref_index);
    }
    else {
      page_locs.check_for_format_changes(epub.get_item_count(), page_id.itemref_index);
    }
    book_viewer.init();
    const PageLocs::PageId * id = page_locs.get_page_id(page_id);
    if (id != nullptr) {
      current_page_id.itemref_index = id->itemref_index;
      current_page_id.offset        = id->offset;
      // show_and_capture(current_page_id);
      return true;
    }
  }
  return false;
}

#if INKPLATE_6PLUS || TOUCH_TRIAL
  void 
  BookController::input_event(const EventMgr::Event & event)
  {
    const PageLocs::PageId * page_id;
    switch (event.kind) {
      case EventMgr::EventKind::SWIPE_RIGHT:
        if (event.y < (Screen::get_height() - 40)) {
          page_id = page_locs.get_prev_page_id(current_page_id);
          if (page_id != nullptr) {
            current_page_id.itemref_index = page_id->itemref_index;
            current_page_id.offset        = page_id->offset;
            show_and_capture(current_page_id);
          }
        }
        else {
          page_id = page_locs.get_prev_page_id(current_page_id, 10);
          if (page_id != nullptr) {
            current_page_id.itemref_index = page_id->itemref_index;
            current_page_id.offset        = page_id->offset;
            show_and_capture(current_page_id);
          }
        }
        break;

      case EventMgr::EventKind::SWIPE_LEFT:
        if (event.y < (Screen::get_height() - 40)) {
          page_id = page_locs.get_next_page_id(current_page_id);
          if (page_id != nullptr) {
            current_page_id.itemref_index = page_id->itemref_index;
            current_page_id.offset        = page_id->offset;
            show_and_capture(current_page_id);
          }
        }
        else {
          page_id = page_locs.get_next_page_id(current_page_id, 10);
          if (page_id != nullptr) {
            current_page_id.itemref_index = page_id->itemref_index;
            current_page_id.offset        = page_id->offset;
            show_and_capture(current_page_id);
          }           
        }
        break;
      
      case EventMgr::EventKind::TAP:
        if (event.y < (Screen::get_height() - 40)) {
          if (event.x < (Screen::get_width() / 3)) {
            page_id = page_locs.get_prev_page_id(current_page_id);
            if (page_id != nullptr) {
              current_page_id.itemref_index = page_id->itemref_index;
              current_page_id.offset        = page_id->offset;
              show_and_capture(current_page_id);
            }
          }
          else if (event.x > ((Screen::get_width() / 3) * 2)) {
            page_id = page_locs.get_next_page_id(current_page_id);
            if (page_id != nullptr) {
              current_page_id.itemref_index = page_id->itemref_index;
              current_page_id.offset        = page_id->offset;
              show_and_capture(current_page_id);
            }
          } else {           
            app_controller.set_controller(AppController::Ctrl::PARAM);
          }
        } else {           
          app_controller.set_controller(AppController::Ctrl::PARAM);
        }
        break;
        
      default:
        break;
    }
  }
#else
  void 
  BookController::input_event(const EventMgr::Event & event)
  {
    const PageLocs::PageId * page_id;
    switch (event.kind) {
      #if EXTENDED_CASE
        case EventMgr::EventKind::DBL_PREV:
      #else
        case EventMgr::EventKind::PREV:
      #endif
        page_id = page_locs.get_prev_page_id(current_page_id);
        if (page_id != nullptr) {
          current_page_id.itemref_index = page_id->itemref_index;
          current_page_id.offset        = page_id->offset;
          show_and_capture(current_page_id);
        }
        break;

      #if EXTENDED_CASE
        case EventMgr::EventKind::PREV:
      #else
        case EventMgr::EventKind::DBL_PREV:
      #endif
        page_id = page_locs.get_prev_page_id(current_page_id, 10);
        if (page_id != nullptr) {
          current_page_id.itemref_index = page_id->itemref_index;
          current_page_id.offset        = page_id->offset;
          show_and_capture(current_page_id);
        }
        break;

      #if EXTENDED_CASE
        case EventMgr::EventKind::DBL_NEXT:
      #else
        case EventMgr::EventKind::NEXT:
      #endif
        page_id = page_locs.get_next_page_id(current_page_id);
        if (page_id != nullptr) {
          current_page_id.itemref_index = page_id->itemref_index;
          current_page_id.offset        = page_id->offset;
          show_and_capture(current_page_id);
        }
        break;

      #if EXTENDED_CASE
        case EventMgr::EventKind::NEXT:
      #else
        case EventMgr::EventKind::DBL_NEXT:
      #endif
        page_id = page_locs.get_next_page_id(current_page_id, 10);
        if (page_id != nullptr) {
          current_page_id.itemref_index = page_id->itemref_index;
          current_page_id.offset        = page_id->offset;
          show_and_capture(current_page_id);
        }
        break;
      
      case EventMgr::EventKind::SELECT:
      case EventMgr::EventKind::DBL_SELECT:
        app_controller.set_controller(AppController::Ctrl::PARAM);
        break;
        
      case EventMgr::EventKind::NONE:
        break;
    }
  }
#endif
