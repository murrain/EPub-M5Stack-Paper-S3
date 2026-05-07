// Copyright (c) 2026 EPub-InkPlate contributors
//
// MIT License. Look at file licenses.txt for details.

#pragma once
#include "global.hpp"

#include "controllers/controller.hpp"
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
class MenuControllerBase : public Controller
{
  public:
    // Run the shared dispatch envelope. Derived classes provide
    // the per-controller hooks below; the envelope itself is
    // identical across all menu screens. `final` because the
    // envelope is intentionally fixed — derived classes should
    // customize via has_active_sub_state / dispatch_to_sub_state,
    // not by overriding the dispatch shell itself.
    void input_event(const EventMgr::Event & event) final;

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
    //
    // skip_strip_refresh: when true, the implementation must
    // not call menu_viewer.show or menu_viewer.clear_highlight
    // on sub-state-completion paths. The caller (typically
    // BooksDirController in the persistent-strip flow) will
    // re-render the screen — including the strip — once the
    // sub-state ends. Without this flag the in-dispatch refresh
    // commits a partial strip update that the outer re-render
    // immediately overwrites, costing one wasted EPD refresh
    // cycle per sub-state completion. Default false preserves
    // the modal AppController-state callers (button builds, in-
    // book menu) where the in-dispatch refresh IS the only
    // thing that re-shows the strip after a form completes.
    virtual void dispatch_to_sub_state(
      const EventMgr::Event & event,
      bool                    skip_strip_refresh = false) = 0;

    // Repaint the full screen state this controller is
    // responsible for: the underlying content (book page for
    // BookParamController, books-dir for OptionController) AND
    // the menu strip on top. Called after a sub-state (form,
    // page-nav) closes — those overlays paint over the entire
    // screen (form_viewer.hpp clears Screen::get_width()-40 ×
    // Screen::get_height()-...), so when they dismiss the
    // book/dir pixels they covered are gone and need to be
    // redrawn. menu_viewer.show alone only repaints the top
    // strip, leaving the form's pixels in the body region —
    // visible as "the book text didn't come back" after closing
    // font settings. Call this from any sub-state-close path
    // that doesn't itself transition out of the controller.
    //
    // Persistent-strip callers (BooksDirController dispatch) skip
    // this, same as they skip menu_viewer.show — the outer
    // re-render owns the repaint in that flow.
    virtual void redraw_underlying_state() = 0;
};
