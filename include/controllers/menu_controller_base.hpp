// Copyright (c) 2026 EPub-InkPlate contributors
//
// MIT License. Look at file licenses.txt for details.

#pragma once
#include "global.hpp"

#include "controllers/event_mgr.hpp"

// Shared dispatch envelope for "menu screen" controllers
// (BookParamController, OptionController). Both controllers run
// the same three-phase event flow:
//
//   1. SWIPE_UP-anywhere → dismiss (only when no sub-state is up)
//   2. If a sub-form / sub-state is active → route the event to it
//   3. Otherwise → menu_viewer.event(); if it returns true (the
//      user picked an action that ends in dismissal), dismiss.
//
// Putting the envelope in this base class means a new gesture or
// a change to the dismissal contract lands in one place. Adding
// a sub-state to a derived controller now requires updating two
// adjacent virtual methods (has_active_sub_state and
// dispatch_to_sub_state) — much harder to forget than the
// previous setup, where each input_event was an independent
// open-coded if-else chain that drifted apart on every change.
class MenuControllerBase
{
  public:
    // Run the shared dispatch envelope. Derived classes provide
    // the per-controller hooks below; the envelope itself is
    // identical across all menu screens.
    void input_event(const EventMgr::Event & event);

  protected:
    // True when any sub-form / sub-state with its own dismiss
    // handshake is active. The envelope uses this answer for
    // two decisions:
    //   - SWIPE_UP-dismiss is suppressed (sub-state owns its
    //     own cancel/OK affordance — bypassing it would orphan
    //     the handshake, e.g. WiFi server still running).
    //   - Events route to dispatch_to_sub_state instead of the
    //     menu viewer.
    // Both decisions read the same answer, so they cannot drift.
    virtual bool has_active_sub_state() const = 0;

    // Dispatch the event to whichever sub-state is currently
    // active. Only called when has_active_sub_state() returned
    // true. The sub-state owns the event entirely — no fall-
    // through to the menu viewer when this is invoked.
    virtual void dispatch_to_sub_state(const EventMgr::Event & event) = 0;

    // Hook called immediately before set_controller(LAST) on
    // dismissal — both the SWIPE_UP path and the menu_viewer-
    // says-done path. Subclass uses this to flush deferred
    // side-effects (OptionController's books_dir.refresh on a
    // queued books_refresh_needed). Default is a no-op so
    // controllers without dismiss-time side-effects don't need
    // to override.
    virtual void on_before_dismiss() {}
};
