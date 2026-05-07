// Copyright (c) 2026 EPub-InkPlate contributors
//
// MIT License. Look at file licenses.txt for details.

#pragma once
#include "global.hpp"

#include "controllers/event_mgr.hpp"

// Common interface for the five top-level controllers
// (BooksDirController, BookController, BookParamController,
// OptionController, TocController) that AppController dispatches
// between.
//
// AppController previously open-coded four parallel switches
// (enter / leave / leave(true) / input_event) over the Ctrl enum
// to dispatch to the right concrete controller. Adding a new
// controller required editing all four sites in lockstep — easy
// to forget, no compile-time enforcement. With the polymorphic
// interface here AppController holds a `Controller * table_[]`
// indexed by Ctrl and dispatches via virtual call. A new
// controller is a one-line edit (add to the table); failing to
// override any of the three pure-virtual methods is a compile
// error.
//
// MenuControllerBase (the BookParam/Option shared envelope) now
// inherits from Controller and re-declares input_event as the
// final override; the menu controllers' enter() and leave() are
// still defined per-derived-class.
//
// Cost: one vtable per concrete controller class (~20 bytes
// total across the five), indirect call per dispatch (~1-2
// cycles, negligible vs. the work each enter/leave does on this
// hardware). Benefit: one source of truth for the dispatch
// shape; compile-time enforcement; no four-place lockstep edit
// when adding controllers.
class Controller
{
  public:
    virtual ~Controller() = default;

    /**
     * @brief Called on transition INTO this controller.
     *
     * Paint the controller's UI, restore any per-session state.
     */
    virtual void enter() = 0;

    /**
     * @brief Called on transition OUT of this controller.
     *
     * @param going_to_deep_sleep true if the device is heading to
     *        deep sleep next; the controller should pause/persist
     *        differently than for a regular controller transition
     *        (e.g. BookController persists last_book to NVS only
     *        on the deep-sleep branch).
     */
    virtual void leave(bool going_to_deep_sleep = false) = 0;

    /**
     * @brief Dispatch a touch / button event to this controller.
     *
     * AppController forwards every input_event to the currently-
     * active controller. The controller decides whether to
     * handle locally, route to a sub-state, or trigger a
     * controller transition via app_controller.set_controller.
     */
    virtual void input_event(const EventMgr::Event & event) = 0;
};
