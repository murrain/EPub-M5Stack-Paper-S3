// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#define __FONTS__ 1
#include "models/fonts.hpp"

#include "models/config.hpp"
#include "models/font_factory.hpp"
#include "models/session_state.hpp"
#include "viewers/msg_viewer.hpp"
#include "viewers/form_viewer.hpp"
#include "controllers/book_param_controller.hpp"
#include "controllers/option_controller.hpp"
#include "helpers/unzip.hpp"
#include "alloc.hpp"
#include "pugixml.hpp"

#include <algorithm>
#include <sys/stat.h>

using namespace pugi;

static constexpr const char * font_fnames[8] = {
  "CrimsonPro",
  "Caladea",
  "Asap",
  "AsapCondensed",
  "DejaVuSerif",
  "DejaVuSerifCondensed",
  "DejaVuSans",
  "DejaVuSansCondensed"
};

static constexpr const char * font_labels[8] = {
  "CRIMSON S",
  "CALADEA S",
  "ASAP",
  "ASAP COND",
  "DEJAVUE S",
  "DEJAVUE COND S",
  "DEJAVU",
  "DEJAVU COND"
};

Fonts::Fonts()
{
  #if USE_EPUB_FONTS
    font_cache.reserve(20);
  #else
    font_cache.reserve(4);
  #endif
}

char *
Fonts::get_file(const char * filename, uint32_t size)
{
  FILE * f = fopen(filename, "r");
  char * buff = nullptr;

  if (f) {
    buff = (char *) allocate(size);
    if (buff && (fread(buff, size, 1, f) != 1)) {
      free(buff);
      buff = nullptr;
    } 
    fclose(f);
  }

  return buff;
}

static uint32_t font_size;
inline bool check_res(xml_parse_result res) { return res.status == status_ok; }

// Per-font (4 styles combined) cumulative size cap, used ONLY
// by the boot-time check_file gate during fonts_list.xml parse.
// This is a "show in font menu / register in font_names[]"
// eligibility check — files larger than the cap are silently
// dropped from the registered list at boot. There is NO runtime
// cap in adjust_default_font / FontFactory::create; if a font
// passes boot registration it can be loaded later regardless of
// runtime memory pressure (the user just gets a load failure
// from FontFactory if RAM is exhausted).
//
// The original 300 KB ceiling was sized for the Inkplate-6 era
// — no PSRAM, tight internal RAM. PaperS3 has 8 MB PSRAM and
// Inkplate-6Plus / TOUCH_TRIAL targets all have substantial
// headroom. Bump to 2 MB on those — accommodates Tinos (~1.7 MB
// unsubsetted) and other modern Latin-extended fonts without
// forcing the user to subset before they can install.
//
// Buttons-only (Inkplate-6 / Inkplate-10) keep the original
// tight cap; those builds can hit memory pressure and the old
// number was a defensive ceiling that's still load-bearing
// there.
#if (INKPLATE_6PLUS || TOUCH_TRIAL)
  static constexpr uint32_t MAX_FONT_SET_SIZE = 1024 * 1024 * 2;  // 2 MB
#else
  static constexpr uint32_t MAX_FONT_SET_SIZE = 1024 * 300;       // 300 KB
#endif

static bool check_file(const std::string & filename)
{
  constexpr const char * TAG = "Check File";
  struct stat file_stat;
  if (!filename.empty()) {
    std::string full_name = std::string(FONTS_FOLDER "/").append(filename);
    if (stat(full_name.c_str(), &file_stat) != -1) {
      font_size += file_stat.st_size;
      if (font_size > MAX_FONT_SET_SIZE) {
        LOG_E("Font size too big for %s", filename.c_str());
      }
      else return true;
    }
    else {
      LOG_E("Font file can't be found: %s", full_name.c_str());
    }
  }
  else {
    LOG_E("Null string...");
  }
  return false;
}

std::string & Fonts::filter_filename(std::string & fname)
{
  std::size_t pos = fname.find("%DPI%");
  if (pos != std::string::npos) {
    char dpi[5];
    int_to_str(Screen::RESOLUTION, dpi, 5);
    fname.replace(pos, 5, dpi);
  }
  return fname;
}

bool Fonts::setup()
{
  FontEntry   font_entry;
  struct stat file_stat;

  LOG_D("Fonts initialization");

  clear_everything();

  constexpr static const char * xml_fonts_descr = MAIN_FOLDER "/fonts_list.xml";

  xml_document     fd;
  char *           fd_data = nullptr;

  if ((stat(xml_fonts_descr, &file_stat) != -1) &&
      ((fd_data = get_file(xml_fonts_descr, file_stat.st_size)) != nullptr) &&
      (check_res(fd.load_buffer_inplace(fd_data, file_stat.st_size)))) {

    LOG_I("Reading fonts definition from fonts_list.xml...");

    // System fonts
    auto sys_group = fd.child("fonts")
                       .find_child_by_attribute("group", "name", "SYSTEM");

    if (sys_group) {
      auto fnt = sys_group.find_child_by_attribute("font", "name", "ICON").child("normal");
      if (!(fnt && add("Icon", 
                       FaceStyle::ITALIC, 
                       filter_filename(std::string(FONTS_FOLDER "/").append(fnt.attribute("filename").value()))))) {
        LOG_E("Unable to find SYSTEM ICON font.");
        return false;
      }

      fnt = sys_group.find_child_by_attribute("font", "name", "TEXT").child("normal");
      if (!(fnt && add("System", 
                      FaceStyle::NORMAL, 
                      filter_filename(std::string(FONTS_FOLDER "/").append(fnt.attribute("filename").value()))))) {
        LOG_E("Unable to find SYSTEM NORMAL font.");
        return false;
      }

      fnt = sys_group.find_child_by_attribute("font", "name", "TEXT").child("italic");
      if (!(fnt && add("System", 
                      FaceStyle::ITALIC, 
                      filter_filename(std::string(FONTS_FOLDER "/").append(fnt.attribute("filename").value()))))) {
        LOG_E("Unable to find SYSTEM ITALIC font.");
        return false;
      }
    }
    else {
      LOG_E("SYSTEM group not found in fonts_list.xml.");
      return false;
    }

    // On warm wake, skip the per-font stat() existence checks. The
    // user font files were validated on the previous cold boot and
    // are unlikely to have vanished while the device was asleep.
    // Each stat on SPI-SD costs roughly 5-15 ms; with 4 stats per
    // font and up to 8 fonts that adds up to a few hundred ms of
    // pure existence-checking on the boot path. We still parse the
    // XML and populate the filename arrays the same way; only the
    // stats are skipped. If a font file actually went missing the
    // failure surfaces later when its TTF is opened for rendering,
    // and the user can resolve it by power-cycling the device which
    // will run the full cold-boot validation.
    const bool skip_font_stats = SessionState::is_warm_wake();
    font_count = 0;
    auto user_group = fd.child("fonts").find_child_by_attribute("group", "name", "USER");
    for (auto fnt : user_group.children("font")) {
      if (font_count >= 8) break;
      std::string str = fnt.attribute("name").value();
      font_size = 0;
      if (!str.empty()) {
        LOG_D("%s...", str.c_str());
        font_names[font_count] = char_pool.set(str);
        str = fnt.child("normal").attribute("filename").value();
        str = filter_filename(str);
        if (skip_font_stats || check_file(str)) {
          regular_fname[font_count] = char_pool.set(str);
          str = fnt.child("bold").attribute("filename").value();
          str = filter_filename(str);
          if (skip_font_stats || check_file(str)) {
            bold_fname[font_count] = char_pool.set(str);
            str = fnt.child("italic").attribute("filename").value();
            str = filter_filename(str);
            if (skip_font_stats || check_file(str)) {
              italic_fname[font_count] = char_pool.set(str);
              str = fnt.child("bold-italic").attribute("filename").value();
              str = filter_filename(str);
              if (skip_font_stats || check_file(str)) {
                bold_italic_fname[font_count] = char_pool.set(str);
                LOG_I("Font %s OK", font_names[font_count]);
                font_count++;
              }
            }
          }
        }
      }
    }

    LOG_D("Got %d fonts from xml file.", font_count);

    if (font_count == 0) {
      LOG_E("No USER font detected!");
      return false;
    }

    FormChoiceField::adjust_font_choices(font_names, font_count);

    book_param_controller.set_font_count(font_count);
        option_controller.set_font_count(font_count);

    int8_t font_index;
    config.get(Config::Ident::DEFAULT_FONT, &font_index);
    if ((font_index < 0) || (font_index >= font_count)) font_index = 0;

    std::string normal      = std::string(FONTS_FOLDER "/").append(    regular_fname[font_index]);
    std::string bold        = std::string(FONTS_FOLDER "/").append(       bold_fname[font_index]);
    std::string italic      = std::string(FONTS_FOLDER "/").append(     italic_fname[font_index]);
    std::string bold_italic = std::string(FONTS_FOLDER "/").append(bold_italic_fname[font_index]);

    if (!add(font_names[font_index], FaceStyle::NORMAL,      normal     )) return false;
    if (!add(font_names[font_index], FaceStyle::BOLD,        bold       )) return false;
    if (!add(font_names[font_index], FaceStyle::ITALIC,      italic     )) return false;
    if (!add(font_names[font_index], FaceStyle::BOLD_ITALIC, bold_italic)) return false;   
    
    fd.reset();
    free(fd_data);
    return true;
  }

  LOG_E("Unable to read fonts definition file fonts_list.xml.");
  return false;
}

Fonts::~Fonts()
{
  for (auto & entry : font_cache) {
    delete entry.font;
  }
  font_cache.clear();
}

void
Fonts::clear(bool all)
{
  std::scoped_lock guard(mutex);
  
  // LOG_D("Fonts Clear!");
  // Keep the first 7 fonts as they are reused. Caches will be cleared.
  #if USE_EPUB_FONTS
    int i = 0;
    for (auto & entry : font_cache) {
      if ((all && (i >= 3)) || (i >= 7)) delete entry.font;
      else entry.font->clear_cache();
      i++;
    }
    font_cache.resize(all ? 3 : 7);
    font_cache.reserve(20);
  #endif
}

void
Fonts::clear_everything()
{
  std::scoped_lock guard(mutex);
  
  // LOG_D("Fonts Clear!");
  // Keep the first 7 fonts as they are reused. Caches will be cleared.
  for (auto & entry : font_cache) {
    delete entry.font;
  }
  font_cache.resize(0);
  font_cache.reserve(20);
}

bool
Fonts::adjust_default_font(uint8_t font_index)
{
  // Guard against a corrupted .pars / config persisting an out-of-range
  // font index. The font_*_fname arrays are sized 8 (the hard cap in the
  // font-loader) and only the first font_count slots are populated.
  // Without this clamp an out-of-range index would read arbitrary memory.
  if (font_index >= font_count) {
    LOG_E("adjust_default_font: index %u out of range (count=%u); using 0",
          (unsigned)font_index, (unsigned)font_count);
    font_index = 0;
  }
  if (font_count == 0) return false;

  // Already on this font — no-op success. The font_cache.at(3)
  // read needs the mutex: a concurrent clear(true) on
  // use_fonts_in_book toggle resizes the vector to 3 entries
  // (line ~298), and an unguarded at(3) would then bounds-fail.
  // Lock just for the read; release before the heavy
  // FontFactory::create calls (those allocate, parse FreeType,
  // potentially do SD I/O — no need to stall other readers
  // through that).
  {
    std::scoped_lock guard(mutex);
    if (font_cache.size() > 3 &&
        font_cache.at(3).name.compare(font_names[font_index]) == 0) {
      return true;
    }
  }

  std::string normal      = std::string(FONTS_FOLDER "/").append(    regular_fname[font_index]);
  std::string bold        = std::string(FONTS_FOLDER "/").append(       bold_fname[font_index]);
  std::string italic      = std::string(FONTS_FOLDER "/").append(     italic_fname[font_index]);
  std::string bold_italic = std::string(FONTS_FOLDER "/").append(bold_italic_fname[font_index]);

  // Transactional swap: pre-load all 4 styles into temporaries before
  // touching the cache. If any one fails (file too large, file missing,
  // FreeType parse error, OTF/CFF table issue from an aggressive
  // subsetting tool, etc.), we delete the loaded temps and leave the
  // cache exactly as it was — the previous default font keeps working.
  //
  // Pre-refactor (replace() chain) this was a half-swap: regular and
  // bold could land at indices 3-4 while italic and bold-italic stayed
  // as the old font at 5-6. Mismatched font metrics across styles
  // broke layout (page_locs computation) and books then "failed to
  // load" — exactly the Tinos symptom that prompted this refactor.
  Font * tmp_normal      = FontFactory::create(normal);
  Font * tmp_bold        = FontFactory::create(bold);
  Font * tmp_italic      = FontFactory::create(italic);
  Font * tmp_bold_italic = FontFactory::create(bold_italic);

  const bool all_loaded =
    (tmp_normal      != nullptr) && tmp_normal     ->is_ready() &&
    (tmp_bold        != nullptr) && tmp_bold       ->is_ready() &&
    (tmp_italic      != nullptr) && tmp_italic     ->is_ready() &&
    (tmp_bold_italic != nullptr) && tmp_bold_italic->is_ready();

  if (!all_loaded) {
    delete tmp_normal;
    delete tmp_bold;
    delete tmp_italic;
    delete tmp_bold_italic;
    LOG_E("adjust_default_font: failed to load %s; cache unchanged",
          font_names[font_index]);
    return false;
  }

  // All 4 loaded. Commit under the cache mutex: free the previous
  // default font's 4 entries and install the new ones at the same
  // indices 3..6.
  std::scoped_lock guard(mutex);

  delete font_cache.at(3).font;
  font_cache.at(3) = { font_names[font_index], tmp_normal,      FaceStyle::NORMAL };
  tmp_normal->set_fonts_cache_index(3);

  delete font_cache.at(4).font;
  font_cache.at(4) = { font_names[font_index], tmp_bold,        FaceStyle::BOLD };
  tmp_bold->set_fonts_cache_index(4);

  delete font_cache.at(5).font;
  font_cache.at(5) = { font_names[font_index], tmp_italic,      FaceStyle::ITALIC };
  tmp_italic->set_fonts_cache_index(5);

  delete font_cache.at(6).font;
  font_cache.at(6) = { font_names[font_index], tmp_bold_italic, FaceStyle::BOLD_ITALIC };
  tmp_bold_italic->set_fonts_cache_index(6);

  LOG_D("Default font is now %s", font_names[font_index]);
  return true;
}

void
Fonts::clear_glyph_caches()
{
  // Acquire the same mutex as clear / clear_everything /
  // adjust_default_font use. Pre-refactor this method walked
  // font_cache without the lock, which would race with the
  // others. Adding here for consistency with the rest of the
  // class — costs a brief uncontended lock acquire.
  std::scoped_lock guard(mutex);
  for (auto & entry : font_cache) {
    entry.font->clear_cache();
  }
}

int16_t
Fonts::get_index(const std::string & name, FaceStyle style)
{
  int16_t idx = 0;

  { std::scoped_lock guard(mutex);

    for (auto & entry : font_cache) {
      if ((entry.name.compare(name) == 0) && 
          (entry.style == style)) return idx;
      idx++;
    }
  }
  return -1;
}

// (Fonts::replace removed: only caller was the pre-transactional
// adjust_default_font which has been rewritten to do its own
// pre-load + atomic swap. Re-introduce if a future use case
// genuinely needs single-slot replacement.)

bool
Fonts::add(const std::string & name, 
           FaceStyle           style,
           const std::string & filename)
{
  std::scoped_lock guard(mutex);
  
  // If the font is already loaded, return promptly
  for (auto & font : font_cache) {
    if ((name.compare(font.name) == 0) && 
        (font.style == style)) return true;
  }

  FontEntry f;
  if ((f.font = FontFactory::create(filename))) {
    if (f.font->is_ready()) {
      f.name  = name;
      f.style = style;
      f.font->set_fonts_cache_index(font_cache.size());
      font_cache.push_back(f);

      LOG_D("Font %s added to cache at index %d and style %d.",
        f.name.c_str(), 
        f.font->get_fonts_cache_index(),
        (int)f.style);
      return true;
    }
    else {
      delete f.font;
    }
  }
  else {
    LOG_E("Unable to allocate memory.");
    // msg_viewer.out_of_memory("font allocation");
  }

  return false;
}

bool 
Fonts::add(const std::string & name, 
           FaceStyle           style,
           unsigned char *     buffer,
           int32_t             size,
           const std::string & filename)
{
  std::scoped_lock guard(mutex);
  
  // If the font is already loaded, return promptly
  for (auto & font : font_cache) {
    if ((name.compare(font.name) == 0) && 
        (font.style == style)) return true;
  }

  FontEntry f;

  if ((f.font = FontFactory::create(filename, buffer, size))) {
    if (f.font->is_ready()) {
      f.name  = name;
      f.style = style;
      f.font->set_fonts_cache_index(font_cache.size());
      font_cache.push_back(f);

      LOG_D("Font %s added to cache at index %d and style %d.",
        f.name.c_str(), 
        f.font->get_fonts_cache_index(),
        (int)f.style);
      return true;
    }
    else {
      delete f.font;
    }
  }
  else {
    LOG_E("Unable to allocate memory.");
    // msg_viewer.out_of_memory("font allocation");
  }

  return false;
}

Fonts::FaceStyle
Fonts::adjust_font_style(FaceStyle style, FaceStyle font_style, FaceStyle font_weight) const
{
  if (font_style == FaceStyle::ITALIC) { 
    // NORMAL -> ITALIC
    // BOLD -> BOLD_ITALIC
    // ITALIC (no change)
    // BOLD_ITALIC (no change)
    if      (style == FaceStyle::NORMAL) style = FaceStyle::ITALIC;
    else if (style == FaceStyle::BOLD  ) style = FaceStyle::BOLD_ITALIC;
  }
  else if (font_style == FaceStyle::NORMAL) { 
    // NORMAL
    // BOLD
    // ITALIC -> NORMAL
    // BOLD_ITALIC -> BOLD
    if      (style == FaceStyle::BOLD_ITALIC) style = FaceStyle::BOLD;
    else if (style == FaceStyle::ITALIC     ) style = FaceStyle::NORMAL;
  }
  if (font_weight == FaceStyle::BOLD) { 
    // NORMAL -> BOLD
    // BOLD
    // ITALIC -> BOLD_ITALIC
    // BOLD_ITALIC
    if      (style == FaceStyle::ITALIC) style = FaceStyle::BOLD_ITALIC;
    else if (style == FaceStyle::NORMAL) style = FaceStyle::BOLD;
  }
  else if (font_weight == FaceStyle::NORMAL) { 
    // NORMAL
    // BOLD -> NORMAL
    // ITALIC
    // BOLD_ITALIC -> ITALIC
    if      (style == FaceStyle::BOLD       ) style = FaceStyle::NORMAL;
    else if (style == FaceStyle::BOLD_ITALIC) style = FaceStyle::ITALIC;
  }

  return style;
}