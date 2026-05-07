// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#pragma once
#include "global.hpp"

#include "controllers/event_mgr.hpp"

#include "screen.hpp"

#include <cstdarg>

/**
 * @brief Message presentation class
 * 
 * This class supply simple alert/info messages presentation to the user.
 * 
 */
class MsgViewer {

  private:
    static constexpr char const * TAG = "MsgViewer";

    // Single rendering implementation. Public show() and typed
    // wrappers all forward here. Takes a va_list so wrappers can
    // forward their variadic args without a second formatting step.
    void vshow(
      MsgType msg_type,
      bool    press_a_key,
      bool    clear_screen,
      const char * title,
      const char * fmt_str,
      va_list      args);

    uint16_t width;
    static constexpr uint16_t HEIGHT  = 300;
    static constexpr uint16_t HEIGHT2 = 450;

    struct DotsZone {
      Pos pos;
      Dim dim;
      int16_t max_dot_count;
      int16_t dots_per_line;
    } dot_zone;

    int16_t dot_count;

    #if INKPLATE_6PLUS || TOUCH_TRIAL
      Pos ok_pos, cancel_pos;
      Dim buttons_dim;
      bool confirmation_required;
    #endif

  public:
    MsgViewer() {};

    enum MsgType { INFO, ALERT, BUG, BOOK, WIFI, NTP_CLOCK, CONFIRM };
    static char icon_char[7];

    // Lowest-level entry point. Direct callers are now rare — the
    // typed wrappers below cover ~80 % of the message shapes used
    // in the codebase. Kept public for the WIFI/NTP/edge cases that
    // don't fit the wrapper presets.
    //
    // Audit 03-viewers.md flagged this as a drift hazard: 30+ call
    // sites with positional bools, and one (option_controller.cpp:609)
    // had already drifted to (false, false) where every neighbor used
    // (false, true). The named wrappers below force the common bool
    // combos through one site each, making future drift impossible
    // for the common cases.
    void show(
      MsgType msg_type,
      bool press_a_key,
      bool clear_screen,
      const char * title,
      const char * fmt_str, ...);

    // ----- Typed wrappers for the common message shapes. -----
    //
    // Naming convention:
    //   show_<kind>            : in-session overlay (clear_screen=false,
    //                            press_a_key=false). Painted on top of
    //                            existing content; brief.
    //   show_<kind>_fullscreen : full-screen splash (clear_screen=true,
    //                            press_a_key=false). At boot or after
    //                            a state-resetting action.
    //   show_<kind>_fatal      : ALERT-only fatal-class fullscreen
    //                            (caller is about to enter deep sleep).
    //   show_confirm           : CONFIRM with the OK/cancel handshake
    //                            (press_a_key=true, clear_screen=false).

    /** ALERT, in-session overlay. clear_screen=false, press_a_key=false. */
    void show_alert(const char * title, const char * fmt_str, ...);

    /** ALERT, fullscreen at boot or before deep sleep. (false, true). */
    void show_alert_fatal(const char * title, const char * fmt_str, ...);

    /** INFO, in-session overlay. (false, false). Most common shape. */
    void show_info(const char * title, const char * fmt_str, ...);

    /** INFO, fullscreen confirmation banner. (false, true). */
    void show_info_fullscreen(const char * title, const char * fmt_str, ...);

    /** BOOK splash for "Loading a book..." style messages. (false, false). */
    void show_book_loading(const char * title, const char * fmt_str, ...);

    /** CONFIRM dialog with OK/cancel. (true, false). Caller drives confirm() afterwards. */
    void show_confirm(const char * title, const char * fmt_str, ...);

    bool confirm(const EventMgr::Event & event, bool & ok);

    //void show_progress(const char * title, ...);
    //void add_dot();
    void out_of_memory(const char * raison);
};

#if __MSG_VIEWER__
  MsgViewer msg_viewer;
#else
  extern MsgViewer msg_viewer;
#endif
