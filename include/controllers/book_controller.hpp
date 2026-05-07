// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#pragma once
#include "global.hpp"

#include "controllers/controller.hpp"
#include "controllers/event_mgr.hpp"
#include "models/epub.hpp"
#include "models/page_locs.hpp"

class BookController : public Controller
{
  public:
    BookController() :
      current_page_id(PageLocs::PageId(0, 0))
    { }

    void input_event(const EventMgr::Event & event) override;
    void enter() override;
    void leave(bool going_to_deep_sleep = false) override;
    bool open_book_file(const std::string & book_title, 
                        const std::string & book_filename, 
                        const PageLocs::PageId & page_id);
    void put_str(const char * str, int xpos, int ypos);

    inline const PageLocs::PageId & get_current_page_id() { return current_page_id; }
    inline void set_current_page_id(const PageLocs::PageId & page_id) { current_page_id = page_id; }

  private:
    static constexpr char const * TAG = "BookController";

    PageLocs::PageId current_page_id;

    // Wraps book_viewer.show_page + WakeSnapshot::capture so the
    // renderer stays decoupled from the books_dir / wake-snapshot
    // subsystems. Every navigation handler in this file should go
    // through here rather than calling book_viewer.show_page
    // directly — that's what keeps the capture coverage complete.
    void show_and_capture(const PageLocs::PageId & page_id);
};

#if __BOOK_CONTROLLER__
  BookController book_controller;
#else
  extern BookController book_controller;
#endif
