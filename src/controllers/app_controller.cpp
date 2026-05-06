// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#define __APP_CONTROLLER__ 1
#include "controllers/app_controller.hpp"

#include "controllers/books_dir_controller.hpp"
#include "controllers/book_controller.hpp"
#include "controllers/book_param_controller.hpp"
#include "controllers/option_controller.hpp"
#include "controllers/toc_controller.hpp"
#include "controllers/event_mgr.hpp"
#include "models/page_cache.hpp"
#include "models/page_locs.hpp"
#include "models/wake_snapshot.hpp"

#if INKPLATE_6PLUS
  #include "controllers/back_lit.hpp"
#endif

#include "screen.hpp"

AppController::AppController() : 
  current_ctrl(Ctrl::DIR),
     next_ctrl(Ctrl::NONE)
{
  for (int i = 0; i < LAST_COUNT; i++) {
    last_ctrl[i] = Ctrl::DIR;
  }
}

void
AppController::start()
{
  current_ctrl = Ctrl::NONE;
  next_ctrl    = Ctrl::DIR;

  #if EPUB_LINUX_BUILD
    launch();
    event_mgr.loop(); // Will start gtk. Will not return.
  #else
    while (true) {
      while (next_ctrl != Ctrl::NONE) launch();
      event_mgr.loop();
    }
  #endif
}

void 
AppController::set_controller(Ctrl new_ctrl) 
{
  LOG_D("===> set_controller()...");
  
  next_ctrl = new_ctrl;
}

void AppController::launch()
{
  #if EPUB_LINUX_BUILD
    if (next_ctrl == Ctrl::NONE) return;
  #endif
  
  Ctrl the_ctrl = next_ctrl;
  next_ctrl = Ctrl::NONE;

  if (((the_ctrl == Ctrl::LAST) && (last_ctrl[0] != current_ctrl)) || (the_ctrl != current_ctrl)) {

    switch (current_ctrl) {
      case Ctrl::DIR:     books_dir_controller.leave(); break;
      case Ctrl::BOOK:         book_controller.leave(); break;
      case Ctrl::PARAM:  book_param_controller.leave(); break;
      case Ctrl::OPTION:     option_controller.leave(); break;
      case Ctrl::TOC:           toc_controller.leave(); break;
      case Ctrl::NONE:
      case Ctrl::LAST:                                  break;
    }

    Ctrl tmp = current_ctrl;
    current_ctrl = (the_ctrl == Ctrl::LAST) ? last_ctrl[0] : the_ctrl;

    if (the_ctrl == Ctrl::LAST) {
      for (int i = 1; i < LAST_COUNT; i++) last_ctrl[i - 1] = last_ctrl[i];
      last_ctrl[LAST_COUNT - 1] = Ctrl::DIR;
    }
    else {
      for (int i = 1; i < LAST_COUNT; i++) last_ctrl[i] = last_ctrl[i - 1];
      last_ctrl[0] = tmp;
    }

    switch (current_ctrl) {
      case Ctrl::DIR:     books_dir_controller.enter(); break;
      case Ctrl::BOOK:         book_controller.enter(); break;
      case Ctrl::PARAM:  book_param_controller.enter(); break;
      case Ctrl::OPTION:     option_controller.enter(); break;
      case Ctrl::TOC:           toc_controller.enter(); break;
      case Ctrl::NONE:
      case Ctrl::LAST:                                  break;
    }
  }
}

void 
AppController::input_event(const EventMgr::Event & event)
{
  if (next_ctrl != Ctrl::NONE) launch();

  #if INKPLATE_6PLUS
    if (event.kind == EventMgr::EventKind::PINCH_ENLARGE) {
      back_lit.adjust(event.dist);
      return;
    }
    else if (event.kind == EventMgr::EventKind::PINCH_REDUCE) {
      back_lit.adjust(-event.dist);
      return;
    }
  #endif

  switch (current_ctrl) {
    case Ctrl::DIR:     books_dir_controller.input_event(event); break;
    case Ctrl::BOOK:         book_controller.input_event(event); break;
    case Ctrl::PARAM:  book_param_controller.input_event(event); break;
    case Ctrl::OPTION:     option_controller.input_event(event); break;
    case Ctrl::TOC:           toc_controller.input_event(event); break;
    case Ctrl::NONE:
    case Ctrl::LAST:                                             break;
  }
}

void
AppController::going_to_deep_sleep()
{
  if (next_ctrl != Ctrl::NONE) launch();

  #if INKPLATE_6PLUS
    back_lit.turn_off();
    touch_screen.shutdown();
  #endif

  // Pause pre-paint BEFORE the persist so enumerate_complete
  // returns stable framebuffer pointers. Without this pause, the
  // pthread could be mid-render of one of the cache slots while
  // persist's fwrite reads it — torn-page-on-disk, fails CRC on
  // next wake's hydrate. Pause is cheap (handshake-bounded, no
  // alloc) and reversed implicitly by the upcoming page_cache.
  // stop() which sees the already-paused state.
  page_cache.pause();

  // Persist the wake snapshot FIRST, while page_cache is still
  // alive. The Phase C multi-page format relies on
  // PageCache::enumerate_complete returning live entries during
  // persist(); calling page_cache.stop() before persist would
  // wipe the slab and the entry table, leaving the v2 file with
  // page_count = 1 (primary only, no pre-painted neighbors).
  //
  // We persist BEFORE the controller leave() callbacks because some
  // of them (book_controller -> save_last_book) write to NVS on the
  // same SPI bus, and ordering the SD write first keeps any later
  // NVS-side hiccup from leaving an orphaned snapshot pointing at
  // a book id whose was_shown bit didn't get set.
  //
  // The capture() into PSRAM happened earlier — every book_viewer.
  // show_page records what's on screen — so this is just the SD-
  // write half of the snapshot path. Skipped if no capture has
  // occurred this session (e.g. user was in the books-dir and
  // never opened a book).
  if (wake_snapshot.has_pending_capture()) {
    wake_snapshot.persist();
  }

  // Now stop pre-paint, AFTER persist has snapshotted the cache.
  // This is the same lock-order discipline used in BooksDir
  // Controller::enter (cache → page_locs → epub) but we don't
  // close the file here because the SleepScreenViewer painted
  // during the leave() switch below still needs fonts alive (see
  // BookController::leave's going_to_deep_sleep=true branch).
  page_cache.stop();

  // Stop the page-locations retriever before any controller writes its
  // own state to flash. Without this, the RetrieverTask can be mid-build
  // and emit a save() of the .locs file (page_locs.cpp computation_completed
  // path) concurrently with book_controller.leave's nvs/locs persistence,
  // corrupting either file. stop_document is safe to call when already
  // idle (the STOP handler short-circuits and sends STOPPED immediately).
  page_locs.stop_document();

  switch (current_ctrl) {
    case Ctrl::DIR:     books_dir_controller.leave(true); break;
    case Ctrl::BOOK:         book_controller.leave(true); break;
    case Ctrl::PARAM:  book_param_controller.leave(true); break;
    case Ctrl::OPTION:     option_controller.leave(true); break;
    case Ctrl::TOC:           toc_controller.leave(true); break;
    case Ctrl::NONE:
    case Ctrl::LAST:                                      break;
  }
}
