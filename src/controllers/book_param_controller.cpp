// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#define __BOOK_PARAM_CONTROLLER__ 1
#include "controllers/book_param_controller.hpp"

#include "controllers/app_controller.hpp"
#include "controllers/common_actions.hpp"
#include "controllers/books_dir_controller.hpp"
#include "controllers/book_controller.hpp"
#include "models/books_dir.hpp"
#include "models/epub.hpp"
#include "models/config.hpp"
#include "models/page_cache.hpp"
#include "models/page_locs.hpp"
#include "models/toc.hpp"
#include "models/wake_snapshot.hpp"
#include "viewers/menu_viewer.hpp"
#include "viewers/form_viewer.hpp"
#include "viewers/msg_viewer.hpp"
#include "viewers/book_viewer.hpp"
#include "viewers/page_nav_viewer.hpp"

#if EPUB_INKPLATE_BUILD && !BOARD_TYPE_PAPER_S3
  #include "esp_system.h"
  #include "eink.hpp"
  #include "esp.hpp"
  #include "soc/rtc.h"
#endif

#include <sys/stat.h>

static int8_t show_images;
static int8_t font_size;
static int8_t use_fonts_in_book;
static int8_t font;
static int8_t done_res;

static int8_t old_font_size;
static int8_t old_show_images;
static int8_t old_use_fonts_in_book;
static int8_t old_font;

#if INKPLATE_6PLUS || TOUCH_TRIAL
  static constexpr int8_t BOOK_PARAMS_FORM_SIZE = 5;
#else
  static constexpr int8_t BOOK_PARAMS_FORM_SIZE = 4;
#endif
static FormEntry book_params_form_entries[BOOK_PARAMS_FORM_SIZE] = {
  { .caption = "Font Size:",
    .u = { .ch = { .value = &font_size,
                   .choice_count = 4,
                   .choices = FormChoiceField::font_size_choices } },
    .entry_type = FormEntryType::HORIZONTAL },
  { .caption = "Use fonts in book:",
    .u = { .ch = { .value = &use_fonts_in_book,
                   .choice_count = 2,
                   .choices = FormChoiceField::yes_no_choices } },
    .entry_type = FormEntryType::HORIZONTAL },
  { .caption = "Font:",
    .u = { .ch = { .value = &font,
                   .choice_count = 8,
                   .choices = FormChoiceField::font_choices } },
    .entry_type = FormEntryType::VERTICAL },
  { .caption = "Show Images in book:",
    .u = { .ch = { .value = &show_images,
                   .choice_count = 2,
                   .choices = FormChoiceField::yes_no_choices } },
    .entry_type = FormEntryType::HORIZONTAL },
  #if INKPLATE_6PLUS || TOUCH_TRIAL
    { .caption = " DONE ",
      .u = { .ch = { .value = &done_res,
                     .choice_count = 0,
                     .choices = nullptr } },
      .entry_type = FormEntryType::DONE }
  #endif
};

static void
book_parameters()
{
  BookParams * book_params = epub.get_book_params();

  book_params->get(BookParams::Ident::SHOW_IMAGES,        &show_images      );
  book_params->get(BookParams::Ident::FONT_SIZE,          &font_size        );
  book_params->get(BookParams::Ident::USE_FONTS_IN_BOOK,  &use_fonts_in_book);
  book_params->get(BookParams::Ident::FONT,               &font             );
  
  if (show_images       == -1) config.get(Config::Ident::SHOW_IMAGES,        &show_images      );
  if (font_size         == -1) config.get(Config::Ident::FONT_SIZE,          &font_size        );
  if (use_fonts_in_book == -1) config.get(Config::Ident::USE_FONTS_IN_BOOKS, &use_fonts_in_book);
  if (font              == -1) config.get(Config::Ident::DEFAULT_FONT,       &font             );
  
  old_show_images        = show_images;
  old_use_fonts_in_book  = use_fonts_in_book;
  old_font               = font;
  old_font_size          = font_size;
  done_res               = 1;

  form_viewer.show(
    book_params_form_entries, 
    BOOK_PARAMS_FORM_SIZE, 
    "(Any item change will trigger book refresh)");

  book_param_controller.set_book_params_form_is_shown();
}

static void
revert_to_defaults()
{
  // (void)-cast: best-effort early stop. The defensive stop at
  // ~line 156 (right before the font mutation) is the safety-
  // critical one; if the retriever is wedged THAT call's bool
  // check refuses the font mutation. This top-of-function call
  // is just a pre-emptive idle.
  (void) page_locs.stop_document();

  EPub::BookFormatParams * book_format_params = epub.get_book_format_params();

  BookParams * book_params = epub.get_book_params();

  old_use_fonts_in_book = book_format_params->use_fonts_in_book;
  old_font              = book_format_params->font;

  constexpr int8_t default_value = -1;

  book_params->put(BookParams::Ident::SHOW_IMAGES,       default_value);
  book_params->put(BookParams::Ident::FONT_SIZE,         default_value);
  book_params->put(BookParams::Ident::FONT,              default_value);
  book_params->put(BookParams::Ident::USE_FONTS_IN_BOOK, default_value);

  // update_book_format_params now handles wake_snapshot.invalidate
  // and page_cache.invalidate_all internally — see the doc comment
  // on the method in epub.hpp for the rationale (was previously
  // open-coded here and forgotten in option_controller, which the
  // overnight audit caught).
  epub.update_book_format_params();

  book_params->save();

  msg_viewer.show_info(
                  "E-book parameters reverted",
                  "E-book parameters reverted to default values.");

  // Defensive re-stop of the retriever immediately before the font
  // mutation, mirroring the form path's "quiesce before font swap"
  // contract at lines 323-326. The earlier stop_document at the top
  // of this function covers the steady state, but msg_viewer.show
  // ran in between and (in principle) could be a future re-arm
  // path; this call is idempotent on already-quiesced state and
  // keeps the safety contract co-located with the site that needs
  // it. invalidate_all is NOT repeated here — line ~141 already
  // dropped every cache entry for the format-param edit, no new
  // entries can have been added between there and here (pre-paint
  // only writes via the prepaint_queue, which msg_viewer.show
  // doesn't enqueue), so a second invalidate_all would walk the
  // already-empty entry table for no benefit.
  // Bool-checked: this is the load-bearing safety stop before
  // the font mutation. If the retriever is wedged, doing
  // fonts.clear / load_fonts / adjust_default_font would UAF on
  // any Font* the retriever still holds via fonts.get(). On
  // failure, log and skip the font-side mutation entirely; the
  // PARS file already reflects the new defaults (book_params->put
  // ran before this) and will take effect on the next book open
  // when the retriever is fresh.
  if (old_use_fonts_in_book != book_format_params->use_fonts_in_book ||
      old_font              != book_format_params->font) {
    if (!page_locs.stop_document()) {
      LOG_E("revert_to_defaults: retriever did not idle; skipping "
            "font-side mutation to avoid UAF. New defaults are in "
            "PARS and will apply on next book open.");
      return;
    }
  }

  if (old_use_fonts_in_book != book_format_params->use_fonts_in_book) {
    if (book_format_params->use_fonts_in_book) {
      epub.load_fonts();
    }
    else {
      fonts.clear();
      fonts.clear_glyph_caches();
    }
  }

  if (old_font != book_format_params->font) {
    // (void)-cast on adjust_default_font: revert_to_defaults is a
    // user-initiated reset; if the new default font fails to load,
    // the cache stays on the previous default (per the doc comment
    // on adjust_default_font) and the user sees the prior font
    // until they pick a different one. No additional handling
    // needed here.
    (void) fonts.adjust_default_font(book_format_params->font);
  }
}

static void 
books_list()
{
  app_controller.set_controller(AppController::Ctrl::DIR);
}

static void
delete_book()
{
  msg_viewer.show_confirm(
                  "Delete e-book", 
                  "The e-book \"%s\" will be deleted. Are you sure?", 
                  epub.get_title());
  book_param_controller.set_delete_current_book();
}

static void 
toc_ctrl()
{
  app_controller.set_controller(AppController::Ctrl::TOC);
}

extern bool start_web_server();
extern bool  stop_web_server();

static void
wifi_mode()
{
  #if EPUB_INKPLATE_BUILD
    // Same UAF guard as option_controller's wifi_mode. quiesce
    // the retriever + pre-paint before tearing down state they
    // hold pointers into.
    if (epub.quiesce_book_session()) {
      epub.close_file();
      fonts.clear(true);
      fonts.clear_glyph_caches();
    }
    else {
      LOG_E("wifi_mode: retriever did not idle; book state remains "
            "resident through wifi session.");
    }

    event_mgr.set_stay_on(true); // DO NOT sleep

    if (start_web_server()) {
      book_param_controller.set_wait_for_key_after_wifi();
    }
  #endif
}

static void
power_off()
{
  books_dir_controller.save_last_book(book_controller.get_current_page_id(), true);

  CommonActions::power_it_off();
}

static void
jump_to_page()
{
  int16_t page_count = page_locs.get_page_count();

  if (page_count <= 0) {
    msg_viewer.show_info(
                    "Jump to Page",
                    "Pages are still being computed. Please wait.");
    return;
  }

  // Get current page for default value (1-based for the viewer)
  const PageLocs::PageId & current_page_id = book_controller.get_current_page_id();
  int16_t current_page = page_locs.get_page_nbr(current_page_id);
  uint16_t start_page = (current_page >= 0) ? static_cast<uint16_t>(current_page + 1) : 1;

  page_nav_viewer.show(start_page, static_cast<uint16_t>(page_count), "Jump to Page");
  book_param_controller.set_page_nav_is_shown();
}

// IMPORTANT!!
// The first (menu[0]) and the last menu entry (the one before END_MENU) MUST ALWAYS BE VISIBLE!!!

static MenuViewer::MenuEntry menu[11] = {
  { MenuViewer::Icon::RETURN,      "Return to the e-books reader",         CommonActions::return_to_last, true , true },
  { MenuViewer::Icon::TOC,         "Table of Content",                     toc_ctrl                     , false, true },
  { MenuViewer::Icon::BOOK,        "Jump to Page Number",                  jump_to_page                 , true , true },
  { MenuViewer::Icon::BOOK_LIST,   "E-Books list",                         books_list                   , true , true },
  { MenuViewer::Icon::FONT_PARAMS, "Current e-book parameters",            book_parameters              , true , true },
  { MenuViewer::Icon::REVERT,      "Revert e-book parameters to "
                                   "default values",                       revert_to_defaults           , true , true },
  { MenuViewer::Icon::DELETE,      "Delete the current e-book",            delete_book                  , true , true },
  { MenuViewer::Icon::WIFI,        "WiFi Access to the e-books folder",    wifi_mode                    , true , true },
  { MenuViewer::Icon::INFO,        "About the EPub-InkPlate application",  CommonActions::about         , true , true },
  { MenuViewer::Icon::POWEROFF,    "Power OFF (Deep Sleep)",               power_off                    , true , true },
  { MenuViewer::Icon::END_MENU,    nullptr,                                nullptr                      , false, true }
}; 

void
BookParamController::set_font_count(uint8_t count)
{
  book_params_form_entries[2].u.ch.choice_count = count;
}

void
BookParamController::enter()
{
  LOG_W("enter: phase: BEGIN");
  LOG_W("enter: phase: toc_check");
  menu[1].visible = toc.is_ready() && !toc.is_empty();
  LOG_W("enter: phase: menu_viewer.show");
  menu_viewer.show(menu);
  LOG_W("enter: phase: form_state_reset");
  book_params_form_is_shown = false;
  LOG_W("enter: phase: DONE");
}

void 
BookParamController::leave(bool going_to_deep_sleep)
{

}

bool
BookParamController::has_active_sub_state() const
{
  // Each flag corresponds to a sub-form / sub-state with its own
  // cancel/OK or key-press handshake. New flags belong here AND
  // in dispatch_to_sub_state below — keep the two methods in sync.
  // wait_for_key_after_wifi is unguarded because the flag is
  // declared unconditionally; it just stays false in non-Inkplate
  // builds (the only setter is inside an EPUB_INKPLATE_BUILD path).
  return book_params_form_is_shown
      || page_nav_is_shown
      || delete_current_book
      || wait_for_key_after_wifi;
}

void
BookParamController::dispatch_to_sub_state(
  const EventMgr::Event & event,
  bool                    /*skip_strip_refresh*/)
{
  // BookParamController is only ever the modal in-book menu —
  // never the inline persistent-strip path — so its menu_viewer
  // refresh calls are always load-bearing. The skip flag exists
  // for protocol uniformity with OptionController; ignore it.
  if (book_params_form_is_shown) {
    if (form_viewer.event(event)) {
      book_params_form_is_shown = false;

      BookParams * book_params = epub.get_book_params();

      // Quiesce the retriever (page_locs) and pre-paint
      // (page_cache) BEFORE swapping the font_cache. Both hold
      // raw Font* pointers obtained via fonts.get(); if they
      // dereference one mid-swap (the transactional commit
      // deletes the previous font_cache.at(3..6).font), it's a
      // UAF. The pre-refactor code happened to do this safely
      // by accident — stop_document ran inside the is_modified
      // block which preceded adjust_default_font. The new
      // ordering puts adjust BEFORE the puts (so we can
      // pre-validate), so we must hoist the stops up too.
      //
      // page_locs.stop_document waits via STOP/STOPPED handshake.
      // page_cache.invalidate_all drops cache entries and pauses
      // pre-paint until the next residency request. Both are
      // O(ms); both are safe to call even if no font change
      // actually happens (idempotent on quiesced state).
      if (old_font != font) {
        if (!page_locs.stop_document()) {
          // Retriever didn't idle; skip the font swap entirely.
          // Show the same UX as a font-load failure (alert +
          // revert local var) so the form's about-to-be-persisted
          // FONT param goes back to the working previous value.
          msg_viewer.show_alert(
                          "Could not apply font change",
                          "Reader is busy. Try again in a moment.");
          font = old_font;
        }
        else {
          page_cache.invalidate_all();
        }
      }

      // Validate the font change BEFORE persisting PARS. The
      // pre-refactor flow saved the new FONT param first, then
      // attempted the load — if any of the 4 styles failed, the
      // PARS file already had the broken font index and the book
      // became unrecoverable without external file editing
      // (user-reported Tinos symptom). adjust_default_font is
      // now transactional + boolean: on failure the cache is
      // left as the previous default and we revert the local
      // `font` so book_params->put doesn't persist the
      // unloadable index.
      if (old_font != font) {
        if (!fonts.adjust_default_font(font)) {
          msg_viewer.show_alert(
                          "Font Load Failed",
                          "Could not load the selected font. "
                          "The previous font is still in use.");
          font = old_font;  // PARS will keep the working previous value.
        }
      }

      if (show_images       !=       old_show_images) book_params->put(BookParams::Ident::SHOW_IMAGES,        show_images      );
      if (font_size         !=         old_font_size) book_params->put(BookParams::Ident::FONT_SIZE,          font_size        );
      if (font              !=              old_font) book_params->put(BookParams::Ident::FONT,               font             );
      if (use_fonts_in_book != old_use_fonts_in_book) book_params->put(BookParams::Ident::USE_FONTS_IN_BOOK,  use_fonts_in_book);

      if (book_params->is_modified()) {
        // (void)-cast: format-params edit (orientation, show_title,
        // show_images, font_size, use_fonts_in_book — but NOT font;
        // font already handled above with bool-check). update_book_
        // format_params just refreshes the cached struct — no Font*
        // dereferences, so a still-live retriever can't UAF here.
        (void) page_locs.stop_document();
        // update_book_format_params now invalidates wake_snapshot
        // and page_cache internally — both reflected the OLD
        // layout and would repaint stale content otherwise. See
        // epub.hpp doc comment.
        epub.update_book_format_params();
      }

      book_params->save();

      if (old_use_fonts_in_book != use_fonts_in_book) {
        if (use_fonts_in_book) {
          epub.load_fonts();
        }
        else {
          fonts.clear();
          fonts.clear_glyph_caches();
        }
      }
      // (Font change handled above, before persisting PARS.)

      menu_viewer.clear_highlight();
    }
  }
  else if (page_nav_is_shown) {
    if (!page_nav_viewer.event(event)) {
      page_nav_is_shown = false;

      if (!page_nav_viewer.was_canceled()) {
        // Convert from 1-based UI to 0-based internal page number
        int16_t target_page_nbr = static_cast<int16_t>(page_nav_viewer.get_value()) - 1;

        const PageLocs::PageId * target_page_id = page_locs.get_page_id_from_page_nbr(target_page_nbr);

        if (target_page_id != nullptr) {
          book_controller.set_current_page_id(*target_page_id);
          app_controller.set_controller(AppController::Ctrl::BOOK);
        }
        else {
          msg_viewer.show_info(
                          "Jump to Page",
                          "Page %d could not be found.",
                          page_nav_viewer.get_value());
        }
      }
      else {
        menu_viewer.show(menu);
      }
    }
  }
  else if (delete_current_book) {
    bool ok;
    if (msg_viewer.confirm(event, ok)) {
      if (ok) {
        std::string filepath = epub.get_current_filename();
        struct stat file_stat;

        if (stat(filepath.c_str(), &file_stat) != -1) {
          LOG_I("Deleting %s...", filepath.c_str());

          // Quiesce before close — the user just confirmed delete-
          // book, so a still-live retriever holding pointers into
          // the about-to-be-freed EPub state would UAF on its next
          // dereference. On timeout, refuse the delete and show an
          // alert; the user can retry once the retriever surfaces.
          if (!epub.quiesce_book_session()) {
            msg_viewer.show_alert(
                "Could not delete book",
                "Reader is busy. Try again in a moment.");
            return;
          }
          epub.close_file();
          unlink(filepath.c_str());

          int16_t pos = filepath.find_last_of('.');

          filepath.replace(pos, 5, ".pars");

          if (stat(filepath.c_str(), &file_stat) != -1) {
            LOG_I("Deleting file : %s", filepath.c_str());
            unlink(filepath.c_str());
          }

          filepath.replace(pos, 5, ".locs");

          if (stat(filepath.c_str(), &file_stat) != -1) {
            LOG_I("Deleting file : %s", filepath.c_str());
            unlink(filepath.c_str());
          }

          filepath.replace(pos, 5, ".toc");

          if (stat(filepath.c_str(), &file_stat) != -1) {
            LOG_I("Deleting file : %s", filepath.c_str());
            unlink(filepath.c_str());
          }

          // Synchronous refresh after a book delete is intentional:
          // force_init=false means refresh() only walks the
          // existing DB to drop the deleted entry's record (no
          // EPUB metadata extraction, no cover decoding). The
          // operation completes in <100ms typically. The async
          // path that CommonActions::refresh_books_dir uses is
          // designed for force_init=true full rescans where
          // USB starvation matters; this short post-delete
          // rescan doesn't justify the worker-pthread overhead.
          int16_t dummy;
          books_dir.refresh(nullptr, dummy, false);

          app_controller.set_controller(AppController::Ctrl::DIR);
        }
      }
      else {
        msg_viewer.show_info(
                        "Canceled", "The e-book was not deleted.");
      }
      delete_current_book = false;
    }
  }
  #if EPUB_INKPLATE_BUILD
    else if (wait_for_key_after_wifi) {
      msg_viewer.show_info_fullscreen(
                      "Restarting",
                      "The device is now restarting. Please wait.");
      wait_for_key_after_wifi = false;
      stop_web_server();
      esp_restart();
    }
  #endif
}
