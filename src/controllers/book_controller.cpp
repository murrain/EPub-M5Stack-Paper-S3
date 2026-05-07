// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#define __BOOK_CONTROLLER__ 1
#include "controllers/book_controller.hpp"

#include "controllers/app_controller.hpp"
#include "controllers/books_dir_controller.hpp"
#include "controllers/gestures.hpp"
#include "models/books_dir.hpp"
#include "models/epub.hpp"
#include "models/page_cache.hpp"
#include "models/session_state.hpp"
#include "models/wake_snapshot.hpp"
#include "screen.hpp"
#include "viewers/book_viewer.hpp"
#include "viewers/page.hpp"
#include "viewers/msg_viewer.hpp"

#include <cstring>

#if EPUB_INKPLATE_BUILD
  #include "nvs.h"
#endif

// Stage 2 PageCache integration helpers
namespace
{
  // Asymmetric residency policy — 4 forward, 1 back, biased toward
  // the dominant reading direction. With SLOT_COUNT=10 and a 6-page
  // set (current + 4 ahead + 1 back), there's headroom for stale
  // entries during navigation drift before eviction kicks in.
  // Tuning these here changes pre-paint workload but not memory
  // budget (cap is in page_cache.hpp).
  //
  // Forward-bias matters most during fast sweeps: a user looking
  // for a passage taps next-next-next 4-5 times in a row. Symmetric
  // ±2 (the previous policy) ran out of forward cache after 2
  // swipes; this gives 4 cache hits in a row before falling off
  // to live render.
  constexpr int RESIDENCY_AHEAD  = 4;
  constexpr int RESIDENCY_BEHIND = 1;

  // Compute the current ± N residency set. Caller (show_and_capture)
  // forwards to page_cache.request_residency. Skips entries when
  // page_locs runs out of pages in either direction; the user is
  // near a book edge and the cache simply holds fewer entries.
  // get_next/prev_page_id are mutex-safe internally.
  void compute_residency_set(const PageLocs::PageId & current,
                             PageLocs::PageId * out,
                             size_t & out_count)
  {
    out_count = 0;
    out[out_count++] = current;
    PageLocs::PageId pid = current;
    for (int k = 0; k < RESIDENCY_AHEAD; ++k) {
      const PageLocs::PageId * next = page_locs.get_next_page_id(pid);
      if (next == nullptr) break;
      out[out_count++] = *next;
      pid = *next;
    }
    pid = current;
    for (int k = 0; k < RESIDENCY_BEHIND; ++k) {
      const PageLocs::PageId * prev = page_locs.get_prev_page_id(pid);
      if (prev == nullptr) break;
      out[out_count++] = *prev;
      pid = *prev;
    }
  }
}

// Single rendering + snapshot-capture path. Every navigation handler
// below goes through here rather than calling book_viewer.show_page
// directly — that keeps BookViewer a pure renderer, ensures every
// painted page is also captured for the warm-wake fast path, and
// (Stage 2) gives a single point where the PageCache cache-hit
// shortcut and the post-display pre-paint trigger live.
void
BookController::show_and_capture(const PageLocs::PageId & page_id)
{
  uint32_t fh = WakeSnapshot::format_params_hash(
                  epub.get_book_format_params(),
                  sizeof(EPub::BookFormatParams));

  // Cache-hit fast path runs lock-free; safety story below.
  //
  // Cache-hit fast path: if the PageCache has a complete pre-paint
  // bitmap for this page, blit it to the panel framebuffer and
  // commit via screen.update(FAST). Cache miss falls through to
  // the existing book_viewer.show_page render path.
  //
  // CONCURRENCY: this path deliberately does NOT take book_viewer.
  // get_mutex(). The original implementation did, to satisfy Phase
  // A's update()-asserts-active==panel invariant. That assertion
  // was protecting against single-thread misuse (a viewer calling
  // update inside its own ScopedRenderTarget guard) — not against
  // cross-thread races. Holding the mutex during memcpy + a
  // ~120 ms MODE_DU waveform also blocked the pre-paint pthread
  // for the entire commit window, which on a fast sweep meant
  // pre-paint never got CPU time to populate ahead of the user.
  //
  // Why dropping the mutex is safe:
  //   - memcpy targets the panel framebuffer directly via
  //     screen.get_panel_framebuffer(); never touches s_active_
  //     framebuffer.
  //   - screen.update reads s_hl's panel-buffer state; never
  //     touches s_active_framebuffer.
  //   - pre-paint's ScopedRenderTarget swaps s_active_framebuffer
  //     to a PSRAM scratch buffer. Foreground memcpy + update
  //     ignore that pointer entirely. So pre-paint can render to
  //     scratch concurrent with this path; the panel sees the
  //     correct cached page; the cache slot we read from is not
  //     evicted until request_residency runs (after this block).
  // The Phase A assertion was relaxed in the same commit to
  // match — see the comment in screen_paper_s3.cpp::update.
  //
  // POINTER LIFETIME: `cached` is the slot's framebuffer pointer
  // returned by page_cache.get under cache_mutex_, then released
  // before the memcpy reads from it. Three potential mutators of
  // that slot:
  //   - pre-paint's render_page_into writes to a slot only when
  //     entries_[slot].complete == false; get() returns nullptr
  //     for incomplete slots, so a "cached != nullptr" hit
  //     guarantees the slot won't be written under us.
  //   - inject_entry can replace any slot but is only called from
  //     WakeSnapshot::hydrate_page_cache during BookController::
  //     open_book_file — never during steady-state reading; we
  //     can't be in show_and_capture for an open book at the
  //     same time as that hydrate path.
  //   - request_residency can evict, but it's called BELOW the
  //     memcpy on this same thread.
  // So `cached` is stable for the duration of this scope.
  bool cache_hit = false;
  LOG_W("show_and_capture: phase: page_cache.get");
  const uint8_t * cached = page_cache.get(page_id, fh);
  if (cached != nullptr) {
    uint8_t * panel_fb = screen.get_panel_framebuffer();
    size_t    fb_size  = screen.get_panel_framebuffer_size();
    if ((panel_fb != nullptr) && (fb_size > 0)) {
      LOG_W("show_and_capture: phase: cache_hit memcpy+update");
      std::memcpy(panel_fb, cached, fb_size);
      // FAST = MODE_DU on PaperS3 (~120 ms). Page::paint normally
      // uses FAST for foreground page turns; the cache path is the
      // same UX but without the layout work.
      screen.update(Screen::UpdateMode::FAST);
      cache_hit = true;
    }
  }
  if (!cache_hit) {
    LOG_W("show_and_capture: phase: book_viewer.show_page (cache miss)");
    book_viewer.show_page(page_id);
    LOG_W("show_and_capture: phase: book_viewer.show_page DONE");
  }

  // Identify the book by its NVS id so the snapshot meta survives the
  // sorted-index reshuffle that read_books_directory does each session.
  // Skip the capture if we somehow paint without an active book — the
  // snapshot would have no meaningful book_id to invalidate against
  // and would just sit on disk shadowing the real (next) book's wake.
  int16_t  bidx = books_dir_controller.get_current_book_index();
  uint32_t book_id = 0;
  if ((bidx >= 0) && books_dir.get_book_id((uint16_t)bidx, book_id)) {
    wake_snapshot.capture(book_id, page_id, fh);
  }

  // Trigger pre-paint of the residency set so the next swipe lands
  // on a cache hit. Done AFTER display so the user-perceived path
  // isn't gated on pre-paint work. request_residency is non-blocking
  // — it just enqueues jobs onto the pre-paint task's queue.
  if (page_cache.is_active()) {
    PageLocs::PageId set[1 + RESIDENCY_AHEAD + RESIDENCY_BEHIND];
    size_t n = 0;
    compute_residency_set(page_id, set, n);
    page_cache.request_residency(set, n, fh);
  }
}

void
BookController::enter()
{
  LOG_D("===> Enter()...");

  // Resume pre-paint pthread (was paused on the way out via leave).
  // Idempotent if cache wasn't paused (e.g. on first entry into
  // BOOK after open_book_file's start()), so always safe to call.
  LOG_W("enter: phase: page_cache.resume");
  page_cache.resume();

  #if DEBUGGING && EPUB_INKPLATE_BUILD
    // Heap watermark on every book enter. Lets a maintainer verify
    // the sequential-load leak fix on a regression-test repro: open
    // book A, back to dir, open book B, etc., and watch that the
    // free / largest-block numbers return to baseline between books.
    // Anything growing monotonically across enters is a regression.
    // Gated on DEBUGGING so release builds don't pay the LOG_I cost.
    ESP::show_internal_heap_info("BookController::enter");
  #endif

  LOG_W("enter: phase: check_for_format_changes");
  page_locs.check_for_format_changes(epub.get_item_count(), current_page_id.itemref_index);
  LOG_W("enter: phase: get_page_id");
  const PageLocs::PageId * id = page_locs.get_page_id(current_page_id);
  if (id != nullptr) {
    current_page_id.itemref_index = id->itemref_index;
    current_page_id.offset        = id->offset;
  }
  else {
    current_page_id.itemref_index = 0;
    current_page_id.offset        = 0;
  }
  LOG_W("enter: phase: show_and_capture (idx=%d off=%d)",
        (int) current_page_id.itemref_index,
        (int) current_page_id.offset);
  show_and_capture(current_page_id);
  LOG_W("enter: phase: DONE");
}

void
BookController::leave(bool going_to_deep_sleep)
{
  LOG_D("===> leave()...");

  // Pause pre-paint pthread on EVERY leave path. Foreground
  // viewers that paint outside book_viewer's mutex (menu_viewer,
  // msg_viewer, battery_viewer, sleep_screen, wallpaper, usb_msc)
  // call screen.update() which asserts that the active framebuffer
  // is the panel — exactly the assertion pre-paint's
  // ScopedRenderTarget violates while it's mid-render. Without
  // this pause, opening the menu while pre-paint is rendering
  // page N+1 abort()s the device. Pause is cheap (~milliseconds
  // of round-trip + at most one in-flight render's worth of time
  // for the inner abort check to fire); resume() reverses it
  // when control comes back to BOOK.
  //
  // Pause happens BEFORE save_last_book / future close_file because
  // we want pre-paint quiesced before any state mutation begins.
  page_cache.pause();

  // Teardown of the active book (page_locs.stop_document +
  // epub.close_file) is NOT done here. AppController::launch calls
  // leave() on every transition out of BOOK including BOOK→PARAM
  // (book parameters editing) and BOOK→TOC, both of which expect
  // the file to remain open — BookParamController reads epub.get_
  // book_params and get_book_format_params, TocController calls
  // epub.retrieve_file for the NCX. Closing the file on those
  // transitions and not reopening it on PARAM/TOC→BOOK return
  // would silently break those flows.
  //
  // Instead, the teardown lives in BooksDirController::enter,
  // which is the natural exit point of a book session — symmetric
  // with how BooksDirController is also where books get opened.
  // The deep-sleep path doesn't transition through DIR; it stops
  // the retriever directly in app_controller.going_to_deep_sleep
  // and lets the PMU power-cut reclaim the rest.
  books_dir_controller.save_last_book(current_page_id, going_to_deep_sleep);
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

  if (new_document) {
    // Lock-order: cache stop, then page_locs stop. Same shape
    // used in BooksDirController::enter for the regular nav-back
    // teardown — both via EPub::quiesce_book_session.
    //
    // Refuse the new open if quiesce times out: epub.open_file
    // close-then-opens internally, and that close would free
    // state still in use by the live retriever. The user lands
    // back on books-dir via show_last_book's existing failure
    // path; trying again in a moment usually succeeds.
    LOG_W("open_book_file: phase: epub.quiesce_book_session");
    if (!epub.quiesce_book_session()) {
      LOG_E("open_book_file: quiesce timed out; refusing to "
            "open new book to avoid retriever UAF on previous "
            "book's state. Retry in a moment.");
      return false;
    }
  }

  LOG_W("open_book_file: phase: epub.open_file");
  if (epub.open_file(book_filename)) {
    if (new_document) {
      LOG_W("open_book_file: phase: page_locs.start_new_document");
      page_locs.start_new_document(epub.get_item_count(), page_id.itemref_index);
      // Re-enable cache for this book. Allocation may fail (PSRAM
      // tight) — that's fine, foreground render still works and
      // is_active() returns false so show_and_capture skips the
      // residency request.
      //
      // INVARIANT: from this point until BookController::enter
      // begins painting (i.e. through the rest of open_book_file's
      // unwind, the launch() handshake, and any caller code that
      // runs between set_controller(BOOK) returning and the run
      // loop dispatching enter), NO FOREGROUND PANEL WRITES are
      // safe in the current controller — the pre-paint pthread is
      // live and holds the panel's render lock for any in-flight
      // ScopedRenderTarget. The Belgariade wedge was a forgotten
      // option_controller.show_menu() in BooksDirController::enter
      // that ran after show_last_book returned and deadlocked on
      // exactly this contention. New callers MUST audit any panel
      // writes that follow set_controller(BOOK).
      LOG_W("open_book_file: phase: page_cache.start");
      if (page_cache.start()) {
        // Phase C warm-wake hydration: if the just-opened book
        // matches the wake_snapshot's persisted book_id and
        // format_hash, inject all the snapshot's non-primary
        // entries into PageCache so the user can swipe to those
        // pages instantly post-wake. Mismatch (different book
        // opened than the one captured at sleep) is a benign
        // skip — pre-paint will repopulate organically as the
        // user navigates.
        //
        // Must happen BEFORE the first show_and_capture (which
        // would request residency and trigger pre-paint of
        // current ± N), otherwise pre-paint would re-render
        // pages we already have on disk from the snapshot.
        uint32_t fh = WakeSnapshot::format_params_hash(
                        epub.get_book_format_params(),
                        sizeof(EPub::BookFormatParams));
        uint32_t book_id = 0;
        int16_t  bidx    = books_dir_controller.get_current_book_index();
        if ((bidx >= 0) &&
            books_dir.get_book_id((uint16_t) bidx, book_id)) {
          wake_snapshot.hydrate_page_cache(book_id, fh);
        }
      }
    }
    else {
      page_locs.check_for_format_changes(epub.get_item_count(), page_id.itemref_index);
    }
    LOG_W("open_book_file: phase: book_viewer.init");
    book_viewer.init();
    LOG_W("open_book_file: phase: page_locs.get_page_id");
    const PageLocs::PageId * id = page_locs.get_page_id(page_id);
    if (id != nullptr) {
      current_page_id.itemref_index = id->itemref_index;
      current_page_id.offset        = id->offset;
      // show_and_capture(current_page_id);
      LOG_W("open_book_file: phase: DONE (success)");
      return true;
    }
    LOG_W("open_book_file: phase: DONE (get_page_id returned null)");
  }
  return false;
}

#if INKPLATE_6PLUS || TOUCH_TRIAL
  void
  BookController::input_event(const EventMgr::Event & event)
  {
    // Top-edge gesture opens the menu (TAP-at-top OR SWIPE_DOWN-
    // from-top). Centralized predicate; see gestures.hpp.
    if (Gestures::is_menu_open(event)) {
      app_controller.set_controller(AppController::Ctrl::PARAM);
      return;
    }

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
        // Top-edge taps were already handled by is_menu_open above.
        // Page navigation columns:
        //   x < width/3     → previous page
        //   x > 2/3 width   → next page
        //   center third    → dead space (no-op, prevents
        //                     accidental menu when reading)
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
