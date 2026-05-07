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
#include "models/epub.hpp"
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
    #if (INKPLATE_6PLUS || TOUCH_TRIAL)
      // Touch builds: events queued during the warm-wake input-
      // dead window (event_mgr.setup() runs before the rest of
      // boot — fonts, page_locs, books_dir scan — and the touch
      // driver pushes events into input_event_queue while the
      // user's swipes, thinking they didn't register, multiply).
      // Without this coalesce, the queued events drain one-at-a-
      // time once the regular event_mgr.loop starts, firing each
      // queued swipe as a separate page turn — page jumps several
      // pages instead of the one the user expected.
      //
      // Drain the queue once and dispatch only the most recent
      // event (latest user intent). Runs only on the first outer-
      // loop pass; after that the queue is drained as events
      // arrive in normal cadence.
      bool first_iteration = true;
    #endif
    while (true) {
      while (next_ctrl != Ctrl::NONE) launch();

      #if (INKPLATE_6PLUS || TOUCH_TRIAL)
        if (first_iteration) {
          first_iteration = false;
          EventMgr::Event coalesced = event_mgr.coalesce_pending_input();
          if (coalesced.kind != EventMgr::EventKind::NONE) {
            input_event(coalesced);
            // continue → re-run the launch-loop in case the
            // dispatched event triggered a controller transition
            // (set_controller sets next_ctrl; only launch() applies
            // it). Without continue we'd block in event_mgr.loop
            // before the transition gets processed.
            continue;
          }
        }
      #endif

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

  // Fire-this-can't-be-missed marker. If "DONE (success)" is the
  // last serial line on a wedged Belgariade open, we need to know
  // whether launch() is even being called. LOG_W is always-on; if
  // this line never prints after a transition was scheduled via
  // set_controller, the run() loop never re-entered launch().
  LOG_W("launch: phase: BEGIN (the_ctrl=%d current_ctrl=%d)",
        (int) the_ctrl, (int) current_ctrl);

  if (((the_ctrl == Ctrl::LAST) && (last_ctrl[0] != current_ctrl)) || (the_ctrl != current_ctrl)) {

    LOG_W("launch: phase: leave_current (current_ctrl=%d)", (int) current_ctrl);
    switch (current_ctrl) {
      case Ctrl::DIR:     books_dir_controller.leave(); break;
      case Ctrl::BOOK:         book_controller.leave(); break;
      case Ctrl::PARAM:  book_param_controller.leave(); break;
      case Ctrl::OPTION:     option_controller.leave(); break;
      case Ctrl::TOC:           toc_controller.leave(); break;
      case Ctrl::NONE:
      case Ctrl::LAST:                                  break;
    }
    LOG_W("launch: phase: leave_done");

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

    LOG_W("launch: phase: enter_new (current_ctrl=%d)", (int) current_ctrl);
    switch (current_ctrl) {
      case Ctrl::DIR:     books_dir_controller.enter(); break;
      case Ctrl::BOOK:         book_controller.enter(); break;
      case Ctrl::PARAM:  book_param_controller.enter(); break;
      case Ctrl::OPTION:     option_controller.enter(); break;
      case Ctrl::TOC:           toc_controller.enter(); break;
      case Ctrl::NONE:
      case Ctrl::LAST:                                  break;
    }
    LOG_W("launch: phase: enter_done");
  }
  else {
    LOG_W("launch: phase: NO_TRANSITION (the_ctrl==current_ctrl)");
  }
  LOG_W("launch: phase: END");
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
  // Then stop the page-locs retriever so it can't write a partial
  // .locs file concurrent with book_controller.leave's nvs/locs
  // persistence (RetrieverTask's computation_completed save()
  // path would otherwise race the leave handler's writes).
  //
  // Same lock-order discipline used elsewhere (cache → page_locs);
  // both wrapped behind EPub::quiesce_book_session. We don't
  // close the file here — SleepScreenViewer painted during the
  // leave() switch below still needs fonts alive (see
  // BookController::leave's going_to_deep_sleep=true branch).
  //
  // Bool return ignored: even on quiesce timeout, we proceed to
  // sleep. The retriever (if still alive) gets power-cut on
  // deep_sleep_now; any partially-written .locs would be
  // detected and rebuilt on next wake.
  (void) epub.quiesce_book_session();

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
