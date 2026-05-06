// Copyright (c) 2026 EPub-InkPlate contributors
//
// MIT License. Look at file licenses.txt for details.

#pragma once
#include "global.hpp"

#include "controllers/event_mgr.hpp"

// Shared gesture vocabulary. Every screen-level controller must
// route its menu-open / menu-dismiss decisions through these
// predicates so the gesture grammar stays consistent across
// screens. New gestures land here once instead of being
// open-coded into each input_event() switch.
//
// Why this exists: SWIPE_DOWN-from-top and SWIPE_UP-anywhere were
// originally added directly to the BookController + BookParam-
// Controller pair, with no obligation to mirror the change to
// the BooksDirController + OptionController pair. The result
// shipped: swipe-to-open and swipe-to-dismiss worked while
// reading a book but not from the books-dir screen. Centralizing
// the predicate makes that asymmetry impossible — every screen
// calls the same function.
namespace Gestures {

  #if INKPLATE_6PLUS || TOUCH_TRIAL

    // Top-edge "drawer" hit region for the menu. ~10% of the
    // 960 px portrait panel — large enough for a thumb, small
    // enough to leave the rest of the page for content taps and
    // horizontal swipes.
    constexpr uint16_t TOP_EDGE_PX = 80;

    // True when the user wants to open the menu from a screen
    // that owns book/dir content — TAP near the top edge or a
    // SWIPE_DOWN that started near the top edge. The y on a
    // SWIPE_DOWN event is the touch START position (see the
    // FSM in event_mgr_paper_s3.cpp), so the same TOP_EDGE_PX
    // gate applies to both gesture kinds.
    //
    // Use is_menu_open_swipe() (below) instead if your screen
    // has tap targets that overlap the top edge — TAP can
    // ambiguously mean "select that thing at the top" or
    // "open the menu," and the screen needs to make that call
    // itself.
    inline bool is_menu_open(const EventMgr::Event & e) {
      return (e.kind == EventMgr::EventKind::TAP ||
              e.kind == EventMgr::EventKind::SWIPE_DOWN) &&
             e.y < TOP_EDGE_PX;
    }

    // Narrow variant: only the SWIPE_DOWN-from-top gesture, NOT
    // a TAP at the top. Use on screens whose top edge contains
    // selectable content — the books-dir matrix view has covers
    // in the first row, and on a linear view the top of the
    // list is also tappable. Treating those taps as "open the
    // menu" steals the user's intended selection. The screen's
    // own hit-test runs first; if no item is hit, the screen
    // can still route the tap to the menu (fallback path) — but
    // a deliberate swipe-down is unambiguous and bypasses the
    // hit-test.
    inline bool is_menu_open_swipe(const EventMgr::Event & e) {
      return e.kind == EventMgr::EventKind::SWIPE_DOWN &&
             e.y < TOP_EDGE_PX;
    }

    // True when the user wants to dismiss the menu. SWIPE_UP
    // anywhere on the screen — the inverse of the drawer-pull
    // gesture that opened it. Callers must still gate on their
    // own sub-form state so a sub-form's own dismiss handshake
    // (form OK/cancel, restart-after-wifi, etc.) isn't bypassed.
    inline bool is_menu_dismiss(const EventMgr::Event & e) {
      return e.kind == EventMgr::EventKind::SWIPE_UP;
    }

  #else

    // Button-only builds (inkplate_6, inkplate_10): the SWIPE_*
    // gesture kinds aren't part of EventKind, and menu open/
    // dismiss happens through physical buttons that the viewer
    // (menu_viewer.event) interprets directly. The predicates
    // exist as always-false stubs so caller code stays build-
    // portable — the compiler folds the dead branch away.
    inline bool is_menu_open(const EventMgr::Event &) { return false; }
    inline bool is_menu_open_swipe(const EventMgr::Event &) { return false; }
    inline bool is_menu_dismiss(const EventMgr::Event &) { return false; }

  #endif

}
