// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#pragma once
#include "global.hpp"

#include "controllers/event_mgr.hpp"
#include "controllers/menu_controller_base.hpp"

class OptionController : public MenuControllerBase
{
  private:
    static constexpr char const * TAG = "OptionController";

    bool main_form_is_shown;
    bool font_form_is_shown;

    #if DATE_TIME_RTC
      bool date_time_form_is_shown;
    #endif
    #if INKPLATE_6PLUS
      bool calibration_is_shown;
    #endif

    bool wait_for_key_after_wifi;

    // Set when the device entered USB Drive Mode (TinyUSB MSC). The
    // exit path mirrors WiFi mode: any input event triggers a clean
    // esp_restart() so the next boot picks up new files via the
    // normal books_dir refresh and the FATFS gets re-mounted from
    // scratch — no in-place teardown of the live MSC stack.
    bool wait_for_key_after_usb;

  public:
    OptionController() : main_form_is_shown(false),
                         font_form_is_shown(false),
                         #if DATE_TIME_RTC
                           date_time_form_is_shown(false),
                         #endif
                         #if INKPLATE_6PLUS
                           calibration_is_shown(false),
                         #endif
                         wait_for_key_after_wifi(false),
                         wait_for_key_after_usb(false) { };

    void          enter() override;
    void          leave(bool going_to_deep_sleep = false) override;
    void set_font_count(uint8_t count);

    // Render the option menu strip. Used by BooksDirController
    // to paint the persistent header on the books-dir screen
    // (the menu is no longer a separate AppController state on
    // touch builds — it lives as a permanent strip and the
    // dispatch goes through BooksDirController::input_event).
    // Internally calls menu_viewer.show(menu) where `menu` is
    // the static option-menu array.
    void show_menu();

    inline void        set_main_form_is_shown() { main_form_is_shown      = true; }
    inline void        set_font_form_is_shown() { font_form_is_shown      = true; }

    #if DATE_TIME_RTC
      inline void set_date_time_form_is_shown() { date_time_form_is_shown = true; }
    #endif

    #if INKPLATE_6PLUS
      inline void    set_calibration_is_shown() { calibration_is_shown    = true; }
    #endif

    inline void set_wait_for_key_after_wifi() {
      wait_for_key_after_wifi   = true;
      main_form_is_shown        = false;
      font_form_is_shown        = false;
      #if DATE_TIME_RTC
        date_time_form_is_shown = false;
      #endif
      #if INKPLATE_6PLUS
        calibration_is_shown    = false;
      #endif
    }

    inline void set_wait_for_key_after_usb() {
      wait_for_key_after_usb    = true;
      main_form_is_shown        = false;
      font_form_is_shown        = false;
      #if DATE_TIME_RTC
        date_time_form_is_shown = false;
      #endif
      #if INKPLATE_6PLUS
        calibration_is_shown    = false;
      #endif
    }

    // Public on this derived class — BooksDirController calls
    // them directly on the global option_controller instance
    // for the persistent-strip dispatch (see BooksDirController
    // ::input_event). Conceptually still implementation details
    // of MenuControllerBase's dispatch envelope (they remain
    // protected on the base — the modal callers reach them via
    // base-class internal dispatch), but OptionController has
    // an additional consumer outside the hierarchy: the screen
    // that hosts the menu as a header. C++ allows widening the
    // access level on override, which is exactly the intent.
    bool has_active_sub_state() const override;
    void dispatch_to_sub_state(
      const EventMgr::Event & event,
      bool                    skip_strip_refresh = false) override;
};

#if __OPTION_CONTROLLER__
  OptionController option_controller;
#else
  extern OptionController option_controller;
#endif
