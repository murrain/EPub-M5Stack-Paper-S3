// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#pragma once
#include "global.hpp"

#include "controllers/event_mgr.hpp"
#include "screen.hpp"

class MenuViewer
{
  public:
    static constexpr uint8_t MAX_MENU_ENTRY = 15;

    enum class Icon { RETURN,      CLR_HISTORY, REFRESH,   BOOK,   BOOK_LIST, MAIN_PARAMS,
                      FONT_PARAMS, POWEROFF,    WIFI,      INFO,   TOC,       DEBUG,
                      DELETE,      CLOCK,       NTP_CLOCK, CALIB,  PREV_MENU, NEXT_MENU, REVERT,
                      USB_DRIVE,
                      END_MENU };
    // One char per Icon enum value (END_MENU excluded). Maps to a
    // glyph in drawings.otf — most letters render as a unique icon
    // baked into the font. USB_DRIVE was added at codepoint 'V' in
    // a font edit alongside this enum extension; bump the array
    // size to match the new entry.
    char icon_char[20] = {
                      '@',         'T',         'R',       'E',    'F',       'C',
                      'A',         'Z',         'S',       'I',    'L',       'H',
                      'K',         'N',         'Y',       'M',    'O',       'P',   'U',
                      'V' };
    struct MenuEntry {
      Icon icon;
      const char * caption;
      void (*func)();
      bool visible;
      bool highlight;
    };
    void  show(MenuEntry * the_menu, uint8_t entry_index = 0, bool clear_screen = false);
    bool event(const EventMgr::Event & event);
    void clear_highlight();

    // Strip height as last computed during show() (instance
    // value). Use compute_region_height() if you need the value
    // before show() has run.
    inline uint16_t get_region_height() const { return region_height; }

    // Compute the strip's rendered height for the current font
    // configuration WITHOUT needing show() to have run first.
    // Mirror of the math in show(): 10 (top padding) + icon
    // glyph height + 10 (icon-to-caption gap) + caption line
    // height + 20 (bottom padding). Returns 0 if the required
    // fonts aren't loaded yet — caller must defer.
    //
    // Why static: the books-dir viewers' setup() needs the strip
    // height to compute their layout (first_entry_ypos), but
    // setup() runs before any menu_viewer.show() — the strip
    // is painted later in BooksDirController::enter(). Without
    // a static way to get the height, the viewers would have
    // to either bake in a hardcoded reservation (the original
    // bug — bootloop when the actual strip overflowed 80) or
    // call show() defensively (extra repaint). The static
    // method is the right shape: same formula, no painting.
    static uint16_t compute_region_height();
    
  private:
    static constexpr char const * TAG = "MenuViewer";

    static const int16_t ICON_SIZE           = 15;
    static const int16_t CAPTION_SIZE        = 12;

    #if INKPLATE_6PLUS
      static const int16_t SPACE_BETWEEN_ICONS = 70;
      static const int16_t ICONS_LEFT_OFFSET   = 20;
    #else
      static const int16_t SPACE_BETWEEN_ICONS = 50;
      static const int16_t ICONS_LEFT_OFFSET   = 10;
    #endif

    uint8_t  current_entry_index;
    uint8_t  max_index;
    uint16_t icon_height, 
             text_height, 
             line_height,
             region_height;
    uint16_t icon_ypos,
             text_ypos;

    #if (INKPLATE_6PLUS || TOUCH_TRIAL)
      bool    hint_shown;
      uint8_t find_index(uint16_t x, uint16_t y);
    #endif

    struct EntryLoc {
      Pos pos;
      Dim dim;
    } entry_locs[MAX_MENU_ENTRY];
    MenuEntry * menu;

    // Render-and-commit helper used by every paint site in the
    // menu (initial show, highlight changes, tap-and-hold hint).
    // Centralizes the partial-paint contract: emit display list
    // to the panel framebuffer, commit ONLY the menu's top strip
    // via Screen::update_region. Book content below the strip
    // stays put on display. On Inkplate / Linux update_region
    // forwards to full update() so the cross-platform path still
    // produces a panel commit.
    void commit_region(Screen::UpdateMode mode,
                       bool clear_screen = false,
                       bool do_it        = false);
};

#if __MENU_VIEWER__
  MenuViewer menu_viewer;
#else
  extern MenuViewer menu_viewer;
#endif
