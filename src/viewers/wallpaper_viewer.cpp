// Copyright (c) 2026 Patrick Smith
//
// MIT License. Look at file licenses.txt for details.

#include "viewers/wallpaper_viewer.hpp"

#if EPUB_INKPLATE_BUILD

#include "screen.hpp"
#include "logging.hpp"

#include <PNGdec.h>
#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include "esp_random.h"

namespace
{
  static constexpr char const * TAG = "WallpaperViewer";

  // Shared buffers for the decode callback — one row each, reused every call.
  // Plain POD arrays — safe as statics (no constructor, zero-init in BSS).
  static uint16_t s_rgb565[540];
  static uint8_t  s_gray8[540];

  // Metadata from the last-shown painting's sidecar .txt file.
  static char s_title[128]       = {};
  static char s_artist_date[128] = {};
  static char s_blurb[512]       = {};

  void clear_metadata()
  {
    s_title[0] = s_artist_date[0] = s_blurb[0] = '\0';
  }

  // Read a sidecar file next to the PNG: 4 lines → title / artist / date / blurb.
  void load_sidecar(const char * png_path)
  {
    clear_metadata();
    char txt_path[256];
    strncpy(txt_path, png_path, sizeof(txt_path) - 1);
    txt_path[sizeof(txt_path) - 1] = '\0';
    char * dot = strrchr(txt_path, '.');
    if (!dot) return;
    strlcpy(dot, ".txt", (size_t)(txt_path + sizeof(txt_path) - dot));

    FILE * f = fopen(txt_path, "r");
    if (!f) return;

    // Four lines: title / artist / date / blurb.
    char artist_buf[128] = {};
    char date_buf[64]    = {};
    char * order[4] = { s_title, artist_buf, date_buf, s_blurb };
    int caps[4]     = { (int)sizeof(s_title), (int)sizeof(artist_buf),
                        (int)sizeof(date_buf), (int)sizeof(s_blurb) };
    for (int i = 0; i < 4; i++) {
      if (!fgets(order[i], caps[i], f)) break;
      char * nl = strchr(order[i], '\n');
      if (nl) *nl = '\0';
    }
    fclose(f);

    // "Artist  ·  Date"
    if (date_buf[0])
      snprintf(s_artist_date, sizeof(s_artist_date), "%s  ·  %s", artist_buf, date_buf);
    else
      strncpy(s_artist_date, artist_buf, sizeof(s_artist_date) - 1);
  }

  // Decode context passed into PNGdec via pUser.
  struct DecCtx {
    PNG *    png;
    int16_t  dst_w;   // target screen width  (540)
    int16_t  dst_h;   // target screen height (960)
  };

  static int png_draw_cb(PNGDRAW * pd)
  {
    DecCtx * ctx = static_cast<DecCtx *>(pd->pUser);
    if (!ctx || pd->y >= ctx->dst_h) return 1;

    // Decode line to RGB565 (handles RGBA/palette/grey source uniformly).
    ctx->png->getLineAsRGB565(pd, s_rgb565, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);

    const int cols = (pd->iWidth < ctx->dst_w) ? pd->iWidth : ctx->dst_w;
    for (int x = 0; x < cols; x++) {
      const uint16_t c = s_rgb565[x];
      const uint8_t  r = (uint8_t)(((c >> 11) & 0x1f) * 8);
      const uint8_t  g = (uint8_t)(((c >>  5) & 0x3f) * 4);
      const uint8_t  b = (uint8_t)((c & 0x1f) * 8);
      // Rec. 601 luma
      s_gray8[x] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
    }

    // Write one row directly to the screen framebuffer.
    screen.draw_bitmap(s_gray8, Dim((uint16_t)cols, 1), Pos(0, (int16_t)pd->y));
    return 1;
  }

  // Collect all *.png entries in dir_path into a vector of full paths.
  std::vector<std::string> list_pngs(const char * dir_path)
  {
    std::vector<std::string> out;
    DIR * d = opendir(dir_path);
    if (!d) return out;
    struct dirent * ent;
    while ((ent = readdir(d)) != nullptr) {
      const char * name = ent->d_name;
      size_t len = strlen(name);
      if (len > 4 && strcasecmp(name + len - 4, ".png") == 0) {
        out.push_back(std::string(dir_path) + "/" + name);
      }
    }
    closedir(d);
    return out;
  }
}

bool WallpaperViewer::show_random(const char * dir_path)
{
  auto files = list_pngs(dir_path);
  if (files.empty()) {
    LOG_D("WallpaperViewer: no PNGs in %s", dir_path);
    return false;
  }

  const uint32_t idx  = esp_random() % (uint32_t)files.size();
  const char *   path = files[idx].c_str();
  load_sidecar(path);
  LOG_I("WallpaperViewer: showing %s", path);

  // Read the whole file into a heap buffer so PNGdec gets a contiguous block.
  FILE * f = fopen(path, "rb");
  if (!f) {
    LOG_E("WallpaperViewer: fopen failed for %s", path);
    return false;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 1024 * 1024) {   // reject > 1 MB
    LOG_E("WallpaperViewer: file size %ld out of range", sz);
    fclose(f);
    return false;
  }

  uint8_t * buf = static_cast<uint8_t *>(malloc((size_t)sz));
  if (!buf) {
    LOG_E("WallpaperViewer: malloc %ld failed", sz);
    fclose(f);
    return false;
  }
  size_t got = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if ((long)got != sz) {
    LOG_E("WallpaperViewer: fread got %zu of %ld bytes for %s", got, sz, path);
    free(buf);
    return false;
  }

  PNG png_obj;
  DecCtx ctx { &png_obj, (int16_t)Screen::get_width(), (int16_t)Screen::get_height() };

  int rc = png_obj.openRAM(buf, (int)sz, png_draw_cb);
  if (rc != PNG_SUCCESS) {
    LOG_E("WallpaperViewer: PNG open error %d for %s", rc, path);
    free(buf);
    return false;
  }

  // decode() passes &ctx as pDraw->pUser into the callback.
  rc = png_obj.decode(&ctx, PNG_FAST_PALETTE);
  free(buf);
  if (rc != PNG_SUCCESS) {
    LOG_E("WallpaperViewer: PNG decode error %d for %s", rc, path);
    clear_metadata();
    return false;
  }
  return true;
}

const char * WallpaperViewer::last_title()       { return s_title;       }
const char * WallpaperViewer::last_artist_date() { return s_artist_date; }
const char * WallpaperViewer::last_blurb()       { return s_blurb;       }

#endif
