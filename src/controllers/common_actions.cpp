// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#define __COMMON_ACTIONS__ 1
#include "controllers/common_actions.hpp"

#include "controllers/app_controller.hpp"
#include "controllers/books_dir_controller.hpp"
#include "controllers/book_controller.hpp"
#include "controllers/event_mgr.hpp"
#include "viewers/msg_viewer.hpp"
#include "viewers/menu_viewer.hpp"
#include "viewers/sleep_screen_viewer.hpp"
#include "models/books_dir.hpp"
#include "models/session_state.hpp"

#if EPUB_INKPLATE_BUILD
  // Async refresh poll loop uses vTaskDelay to yield to TinyUSB
  // and panel-paint tasks while the worker pthread runs the
  // SD scan.
  #include <freertos/FreeRTOS.h>
  #include <freertos/task.h>
#else
  // Linux build: std::this_thread::sleep_for instead.
  #include <chrono>
  #include <thread>
#endif

#if EPUB_INKPLATE_BUILD
  #include "inkplate_platform.hpp"
  #include "esp.hpp"
#endif

void
CommonActions::return_to_last()
{
  app_controller.set_controller(AppController::Ctrl::LAST);
}

void
CommonActions::show_last_book()
{
  books_dir_controller.show_last_book();
}

void
CommonActions::refresh_books_dir()
{
  // Asynchronous refresh — worker pthread does the SD scan and
  // metadata extraction, main task blocks here in a yielding
  // poll loop. With the synchronous refresh that lived here
  // before, a multi-book scan starved TinyUSB (USB host dropped
  // the device after ~16 s) and froze the UI. The async pattern
  // gives the scheduler explicit yield slots so TinyUSB and the
  // panel-paint pthreads keep running while the worker grinds
  // through EPUBs.
  //
  // The msg_viewer banner painted earlier (inside the refresh
  // function on first new-book detection) stays visible through
  // the entire scan — no per-book progress updates because each
  // would cost a ~700 ms full-panel paint cycle and the banner
  // alone is enough to tell the user "device is working." A
  // future iteration could add a small progress region update
  // (page.paint_region with a counter / current filename) if
  // the static banner feels too quiet.
  if (!books_dir.start_async_refresh(true)) {
    // Already in progress — caller raced with itself somehow;
    // log and bail. UI state is whatever the prior refresh left.
    LOG_W("refresh_books_dir: refresh already in progress");
    return;
  }

  while (!books_dir.poll_async_refresh()) {
    // Yield 100 ms per spin. TinyUSB needs to service its host
    // ~every second; 100 ms is comfortably within that window.
    // The worker isn't doing the polling — it's doing actual
    // work — so the delay is purely "main task gives the
    // scheduler room to run other tasks."
    #if EPUB_INKPLATE_BUILD
      vTaskDelay(pdMS_TO_TICKS(100));
    #else
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    #endif
  }

  // refresh() return value is captured but currently unused. If
  // a future caller needs to react to a failed refresh
  // (corrupt SD, OOM, etc), it can read books_dir.async_refresh
  // _result(). For now the LOG_E inside refresh is the only
  // surface for the failure case.

  #if (INKPLATE_6PLUS || TOUCH_TRIAL)
    // Drain any input events the user fired during the scan.
    // Without this drain, the queued taps would dispatch
    // immediately after refresh_view repaints, opening books
    // or menu items the user never meant to tap. See
    // books_dir_controller.cpp::refresh_view for the in-place
    // redraw path that replaces the OPTION→DIR transition the
    // synchronous flow used to rely on.
    {
      EventMgr::Event drained = event_mgr.coalesce_pending_input();
      (void) drained;  // discard — we don't want to honor
                       // post-refresh ghost taps.
    }
    books_dir_controller.refresh_view();
  #else
    // Button builds: action was dispatched from modal OPTION;
    // transitioning back to DIR triggers DIR.enter which
    // redraws the list as a side effect of the leave/enter
    // dance.
    app_controller.set_controller(AppController::Ctrl::DIR);
  #endif
}

void
CommonActions::power_it_off()
{
  #if INKPLATE_6PLUS
    #define MSG "Please press the WakUp Button to restart the device."
    #define INT_PIN TouchScreen::INTERRUPT_PIN
    #define LEVEL 0
  #else
    #define MSG "Please press a key to restart the device."
    #define LEVEL 1
    #if EXTENDED_CASE
      #define INT_PIN PressKeys::INTERRUPT_PIN
    #else
      #define INT_PIN TouchKeys::INTERRUPT_PIN
    #endif
  #endif

  #if defined(BOARD_TYPE_PAPER_S3)
    #undef INT_PIN
    #define INT_PIN ((gpio_num_t)0)
  #endif

  #if EPUB_INKPLATE_BUILD
    // Paint the sleep screen FIRST, before the slow cleanup work
    // below. Reasoning: the user just tapped "Power Off" and
    // expects immediate feedback. With sleep_screen.show running
    // last, the device sat with the menu still on screen for ~2-
    // 3 s of wake_snapshot.persist + page_cache teardown — which
    // looked exactly like a freeze. Painting the wallpaper first
    // gives instant "device went to sleep" confirmation; the
    // teardown work then runs invisibly behind the already-
    // committed final frame.
    //
    // Safe ordering: by the time we get here from the option-
    // menu, the user already left BOOK (BookController::leave
    // fired on the transition into OPTION), which triggered
    // page_cache.pause(). So pre-paint pthread is quiesced —
    // SleepScreenViewer's paint won't race the ScopedRenderTarget
    // assertion.
    SleepScreenViewer::show();

    // Now the heavy-lifting cleanup. wake_snapshot.persist alone
    // is ~1-2 s of SD write, page_cache.stop joins the pthread,
    // page_locs.stop_document blocks on STOPPED ack, controllers
    // each save their last-state. All of this runs while the
    // panel's GC16 waveform from sleep_screen.show is still
    // clocking out — they overlap and the user sees only the
    // wallpaper throughout.
    app_controller.going_to_deep_sleep();

    // Tail delay — only if the cleanup above somehow completed
    // before the ~700 ms GC16 waveform did. With persist in the
    // path that's effectively impossible, but a small defensive
    // delay costs nothing and protects against future paths
    // where cleanup is faster (e.g. a sleep from the books-dir
    // with no book open and no cache to persist).
    ESP::delay(200);

    // Persist the deep-sleep marker AFTER all rendering is committed
    // and just before the actual sleep call. Anything that goes wrong
    // before this point still leaves the next boot in cold-boot state.
    SessionState::mark_entering_deep_sleep();
    inkplate_platform.deep_sleep(INT_PIN, LEVEL);
  #else
    app_controller.going_to_deep_sleep();
    extern void exit_app();
    exit_app();
    exit(0);
  #endif
}

void
CommonActions::about()
{
  // Same waste pattern as init_nvs: msg_viewer.show takes over
  // the screen, so a prior strip-only DU to clear the highlight
  // is immediately overwritten. hint_shown stays true until the
  // next MenuViewer::show resets it (DIR re-entry or post-form),
  // which is harmless — the flag has no direct display effect.
  msg_viewer.show(
    MsgViewer::MsgType::BOOK,
    false,
    false,
    "About EPub-InkPlate",
    "EPub EBook Reader Version %s for the InkPlate e-paper display devices. "
    "This application was made by Guy Turcotte, Quebec, QC, Canada, "
    "with great support from e-Radionica.",
    APP_VERSION);
}
