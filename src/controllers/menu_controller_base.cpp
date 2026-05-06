// Copyright (c) 2026 EPub-InkPlate contributors
//
// MIT License. Look at file licenses.txt for details.

#include "controllers/menu_controller_base.hpp"

#include "controllers/app_controller.hpp"
#include "controllers/gestures.hpp"
#include "viewers/menu_viewer.hpp"

void
MenuControllerBase::input_event(const EventMgr::Event & event)
{
  // SWIPE_UP-anywhere dismisses the menu, but only when no
  // sub-state is up — sub-states (forms, restart prompts, etc.)
  // own their own cancel/OK handshake and must not be bypassed.
  if (Gestures::is_menu_dismiss(event) && !has_active_sub_state()) {
    menu_viewer.clear_highlight();
    app_controller.set_controller(AppController::Ctrl::LAST);
    return;
  }

  // Active sub-state owns the event in full. The derived class's
  // override decides how to route it (form_viewer.event,
  // page_nav_viewer.event, msg_viewer.confirm, etc.) and clears
  // its own state flag when the sub-state completes.
  if (has_active_sub_state()) {
    dispatch_to_sub_state(event);
    return;
  }

  // Top-level menu: if menu_viewer.event returns true, the user
  // selected an action that ends in returning to the previous
  // controller (typically because the action callback fired and
  // the menu is done). Mirror the SWIPE_UP path's clear_highlight
  // so both dismiss paths look identical to a future maintainer —
  // today menu_viewer.event only returns true on the button-build
  // DBL_SELECT path, but tying the two paths together prevents
  // drift if a future touch-build action ever returns true.
  if (menu_viewer.event(event)) {
    menu_viewer.clear_highlight();
    app_controller.set_controller(AppController::Ctrl::LAST);
  }
}
