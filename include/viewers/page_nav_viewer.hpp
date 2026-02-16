// Copyright (c) 2024 Patrick
//
// MIT License. Look at file licenses.txt for details.

#pragma once

#include "global.hpp"
#include "controllers/event_mgr.hpp"
#include "viewers/page.hpp"
#include "viewers/keypad_viewer.hpp"
#include "viewers/screen_bottom.hpp"
#include "models/fonts.hpp"
#include "screen.hpp"

#include <sstream>

class PageNavViewer
{
  public:
    uint16_t get_value()    { return current_value; }
    bool     was_canceled() { return canceled;       }

  private:
    static constexpr char const * TAG = "PageNavViewer";
    static constexpr uint8_t FONT_SIZE = 9;

    // Margins and sizing
    static constexpr uint16_t MARGIN        = 30;
    static constexpr uint16_t TRACK_HEIGHT  =  8;
    static constexpr uint16_t THUMB_WIDTH   = 12;
    static constexpr uint16_t THUMB_HEIGHT  = 28;
    static constexpr uint16_t BTN_PADDING   = 15;
    static constexpr uint16_t ROW_SPACING   = 20;

    // State
    uint16_t current_value;   // 1-based page number
    uint16_t max_value;       // total page count
    bool     canceled;
    bool     keypad_active;

    // Layout (computed in show())
    uint16_t scr_w, scr_h;

    // Overlay
    Pos overlay_pos;
    Dim overlay_dim;

    // Slider track
    Pos track_pos;
    Dim track_dim;
    uint16_t track_inner_left;   // first usable x inside track
    uint16_t track_inner_width;  // usable width for thumb positioning

    // Arrow buttons: ◀◀  ◀  [number]  ▶  ▶▶
    struct Button {
      Pos pos;
      Dim dim;
    };

    Button btn_prev10, btn_prev1, btn_next1, btn_next10;
    Button btn_number;  // tappable page number in center
    Button btn_done, btn_cancel;

    // Vertical positions of each row
    uint16_t title_y;
    uint16_t slider_y;
    uint16_t arrows_y;
    uint16_t progress_y;
    uint16_t buttons_y;

    Page::Format  fmt;
    Font        * font;

    // --- Hit detection ---

    enum class HitZone { 
      NONE, SLIDER, PREV10, PREV1, NUMBER, NEXT1, NEXT10, DONE, CANCEL 
    };

    bool in_button(const Button & btn, uint16_t x, uint16_t y) {
      return x >= btn.pos.x && x <= (btn.pos.x + btn.dim.width) &&
             y >= btn.pos.y && y <= (btn.pos.y + btn.dim.height);
    }

    HitZone hit_test(uint16_t x, uint16_t y) {
      // Slider — generous vertical hit zone
      if (x >= track_pos.x && x <= (track_pos.x + track_dim.width) &&
          y >= (track_pos.y - 15) && y <= (track_pos.y + track_dim.height + 15)) {
        return HitZone::SLIDER;
      }
      if (in_button(btn_prev10, x, y)) return HitZone::PREV10;
      if (in_button(btn_prev1,  x, y)) return HitZone::PREV1;
      if (in_button(btn_number, x, y)) return HitZone::NUMBER;
      if (in_button(btn_next1,  x, y)) return HitZone::NEXT1;
      if (in_button(btn_next10, x, y)) return HitZone::NEXT10;
      if (in_button(btn_done,   x, y)) return HitZone::DONE;
      if (in_button(btn_cancel, x, y)) return HitZone::CANCEL;
      return HitZone::NONE;
    }

    // --- Value helpers ---

    void clamp_value() {
      if (current_value < 1)         current_value = 1;
      if (current_value > max_value) current_value = max_value;
    }

    uint16_t pos_to_page(uint16_t x) {
      if (x <= track_inner_left) return 1;
      if (x >= track_inner_left + track_inner_width) return max_value;
      uint32_t relative = x - track_inner_left;
      return static_cast<uint16_t>(1 + (relative * (max_value - 1)) / track_inner_width);
    }

    uint16_t page_to_thumb_x() {
      if (max_value <= 1) return track_inner_left;
      return static_cast<uint16_t>(
        track_inner_left + 
        ((static_cast<uint32_t>(current_value - 1) * track_inner_width) / (max_value - 1))
      );
    }

    // --- Drawing helpers ---

    void draw_button(const Button & btn, const char * label, bool thick = false) {
      page.put_highlight(btn.dim, btn.pos);
      if (thick) {
        page.put_highlight(
          Dim(btn.dim.width - 2, btn.dim.height - 2),
          Pos(btn.pos.x + 1,     btn.pos.y + 1));
      }
      page.clear_highlight(
        Dim(btn.dim.width - 4, btn.dim.height - 4),
        Pos(btn.pos.x + 2,     btn.pos.y + 2));
      
      Font::Glyph * glyph = font->get_glyph('M', FONT_SIZE);
      page.put_str_at(label,
                      Pos(btn.pos.x + (btn.dim.width >> 1),
                          btn.pos.y + (glyph->dim.height >> 1) + (btn.dim.height >> 1)),
                      fmt);
    }

    void draw_slider() {
      // Clear slider region
      page.clear_region(
        Dim(track_dim.width + 4, THUMB_HEIGHT + 4),
        Pos(track_pos.x - 2,     track_pos.y - ((THUMB_HEIGHT - TRACK_HEIGHT) >> 1) - 2));

      // Draw track (black outline, white interior)
      page.put_highlight(track_dim, track_pos);
      page.clear_highlight(
        Dim(track_dim.width - 4, track_dim.height - 4),
        Pos(track_pos.x + 2,     track_pos.y + 2));

      // Draw thumb
      uint16_t thumb_x = page_to_thumb_x();
      uint16_t thumb_y = track_pos.y - ((THUMB_HEIGHT - TRACK_HEIGHT) >> 1);
      page.put_highlight(
        Dim(THUMB_WIDTH, THUMB_HEIGHT),
        Pos(thumb_x - (THUMB_WIDTH >> 1), thumb_y));
    }

    void draw_value_row() {
      Font::Glyph * glyph = font->get_glyph('M', FONT_SIZE);
      uint16_t text_y = arrows_y + (glyph->dim.height >> 1) + (btn_number.dim.height >> 1);

      // Clear the number area and redraw
      page.clear_region(btn_number.dim, btn_number.pos);

      // Page number text
      char val_str[8];
      int_to_str(current_value, val_str, 8);
      page.put_str_at(val_str,
                      Pos(btn_number.pos.x + (btn_number.dim.width >> 1), text_y),
                      fmt);

      // Progress text below arrows
      page.clear_region(
        Dim(overlay_dim.width - 40, glyph->dim.height + 10),
        Pos(overlay_pos.x + 20,     progress_y - 5));

      std::ostringstream ostr;
      int percentage = (max_value > 0) ? (current_value * 100 / max_value) : 0;
      ostr << current_value << " / " << max_value << "  (" << percentage << "%)";
      page.put_str_at(ostr.str(),
                      Pos(scr_w >> 1, progress_y + (glyph->dim.height >> 1)),
                      fmt);
    }

    void update_display() {
      page.start(fmt);
      draw_slider();
      draw_value_row();
      page.paint(false);
    }

  public:

    void show(uint16_t value, uint16_t page_count, const char * caption) {
      current_value  = value;
      max_value      = page_count;
      canceled       = false;
      keypad_active  = false;

      clamp_value();

      scr_w = Screen::get_width();
      scr_h = Screen::get_height();

      font = fonts.get(1);
      Font::Glyph * glyph = font->get_glyph('M', FONT_SIZE);
      uint16_t line_h = glyph->dim.height + 10;
      uint16_t btn_h  = glyph->dim.height + BTN_PADDING * 2;

      // Compute overlay — centered on screen, sized to fit content
      uint16_t content_width  = scr_w - (MARGIN * 2);
      uint16_t content_height = line_h          // title
                              + ROW_SPACING
                              + THUMB_HEIGHT    // slider
                              + ROW_SPACING
                              + btn_h           // arrow buttons row
                              + ROW_SPACING
                              + line_h          // progress text
                              + ROW_SPACING
                              + btn_h           // done/cancel
                              + ROW_SPACING;

      uint16_t overlay_top = (scr_h >> 1) - (content_height >> 1) - 20;

      overlay_pos = Pos(MARGIN - 10, overlay_top);
      overlay_dim = Dim(content_width + 20, content_height + 40);

      // Compute vertical positions for each row
      uint16_t y = overlay_top + 20;

      title_y = y;
      y += line_h + ROW_SPACING;

      slider_y = y;
      y += THUMB_HEIGHT + ROW_SPACING;

      arrows_y = y;
      y += btn_h + ROW_SPACING;

      progress_y = y;
      y += line_h + ROW_SPACING;

      buttons_y = y;

      // Slider track — full width minus margins
      uint16_t track_left = MARGIN + 10;
      uint16_t track_w    = content_width - 20;
      track_pos = Pos(track_left, slider_y + ((THUMB_HEIGHT - TRACK_HEIGHT) >> 1));
      track_dim = Dim(track_w, TRACK_HEIGHT);
      track_inner_left  = track_left + (THUMB_WIDTH >> 1);
      track_inner_width = track_w - THUMB_WIDTH;

      // Arrow buttons row — evenly spaced across width
      // Layout: [◀◀] [◀]  {page number}  [▶] [▶▶]
      uint16_t arrow_btn_w  = glyph->dim.width * 3 + BTN_PADDING * 2;  // fits "<<"
      uint16_t number_w     = glyph->dim.width * 5 + BTN_PADDING * 2;  // fits "99999"

      uint16_t total_btns_w = arrow_btn_w * 4 + number_w;
      uint16_t spacing      = (content_width - total_btns_w) / 4;

      uint16_t bx = MARGIN;
      btn_prev10 = { Pos(bx, arrows_y), Dim(arrow_btn_w, btn_h) };
      bx += arrow_btn_w + spacing;
      btn_prev1  = { Pos(bx, arrows_y), Dim(arrow_btn_w, btn_h) };
      bx += arrow_btn_w + spacing;
      btn_number = { Pos(bx, arrows_y), Dim(number_w, btn_h) };
      bx += number_w + spacing;
      btn_next1  = { Pos(bx, arrows_y), Dim(arrow_btn_w, btn_h) };
      bx += arrow_btn_w + spacing;
      btn_next10 = { Pos(bx, arrows_y), Dim(arrow_btn_w, btn_h) };

      // Done / Cancel buttons — centered
      uint16_t action_btn_w = glyph->dim.width * 8 + BTN_PADDING * 2;
      uint16_t action_gap   = 40;
      uint16_t action_total = action_btn_w * 2 + action_gap;
      uint16_t action_left  = (scr_w >> 1) - (action_total >> 1);

      btn_done   = { Pos(action_left, buttons_y), Dim(action_btn_w, btn_h) };
      btn_cancel = { Pos(action_left + action_btn_w + action_gap, buttons_y), Dim(action_btn_w, btn_h) };

      // Format for text rendering
      fmt = {
        .line_height_factor =   1.0,
        .font_index         =     1,
        .font_size          = FONT_SIZE,
        .indent             =     0,
        .margin_left        =     0,
        .margin_right       =     0,
        .margin_top         =     0,
        .margin_bottom      =     0,
        .screen_left        =    10,
        .screen_right       =    10,
        .screen_top         =     0,
        .screen_bottom      =     0,
        .width              =     0,
        .height             =     0,
        .vertical_align     =     0,
        .trim               =  true,
        .pre                = false,
        .font_style         = Fonts::FaceStyle::NORMAL,
        .align              = CSS::Align::CENTER,
        .text_transform     = CSS::TextTransform::NONE,
        .display            = CSS::Display::INLINE
      };

      // --- Paint full widget ---

      page.start(fmt);

      // Background
      page.clear_region(overlay_dim, overlay_pos);
      page.put_highlight(
        Dim(overlay_dim.width - 4, overlay_dim.height - 4),
        Pos(overlay_pos.x + 2,     overlay_pos.y + 2));
      page.clear_highlight(
        Dim(overlay_dim.width - 8, overlay_dim.height - 8),
        Pos(overlay_pos.x + 4,     overlay_pos.y + 4));

      // Title
      page.put_str_at(caption,
                      Pos(scr_w >> 1, title_y + (glyph->dim.height >> 1)),
                      fmt);

      // Slider
      draw_slider();

      // Arrow buttons
      draw_button(btn_prev10, "<<");
      draw_button(btn_prev1,  "<");
      draw_button(btn_next1,  ">");
      draw_button(btn_next10, ">>");

      // Page number (center, no border — just the number)
      draw_value_row();

      // Done / Cancel
      draw_button(btn_done,   "DONE",   true);
      draw_button(btn_cancel, "CANCEL", true);

      page.paint(false);
    }

    bool event(const EventMgr::Event & event) {
      // If keypad is active, forward events to it
      if (keypad_active) {
        if (!keypad_viewer.event(event)) {
          // Keypad done
          keypad_active = false;
          uint16_t v = keypad_viewer.get_value();
          if (v >= 1 && v <= max_value) {
            current_value = v;
          }
          clamp_value();
          // Redraw the full widget (keypad overlay needs to be cleared)
          show(current_value, max_value, "Jump to Page");
        }
        return true;
      }

      if (event.kind != EventMgr::EventKind::TAP) return true;

      HitZone zone = hit_test(event.x, event.y);

      switch (zone) {
        case HitZone::SLIDER:
          current_value = pos_to_page(event.x);
          clamp_value();
          update_display();
          break;

        case HitZone::PREV10:
          current_value = (current_value > 10) ? current_value - 10 : 1;
          update_display();
          break;

        case HitZone::PREV1:
          if (current_value > 1) current_value--;
          update_display();
          break;

        case HitZone::NEXT1:
          if (current_value < max_value) current_value++;
          update_display();
          break;

        case HitZone::NEXT10:
          current_value = (current_value + 10 <= max_value) ? current_value + 10 : max_value;
          update_display();
          break;

        case HitZone::NUMBER:
          keypad_active = true;
          keypad_viewer.show(current_value, "Page Number");
          break;

        case HitZone::DONE:
          canceled = false;
          return false;

        case HitZone::CANCEL:
          canceled = true;
          return false;

        case HitZone::NONE:
          break;
      }

      return true;
    }
};

#if __PAGE_NAV_VIEWER__
  PageNavViewer page_nav_viewer;
#else
  extern PageNavViewer page_nav_viewer;
#endif
