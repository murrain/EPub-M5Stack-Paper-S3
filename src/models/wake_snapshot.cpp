// Copyright (c) 2026 Patrick Smith
//
// MIT License. Look at file licenses.txt for details.

#define __WAKE_SNAPSHOT__ 1
#include "models/wake_snapshot.hpp"

#include "screen.hpp"
#include "logging.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>

#if EPUB_INKPLATE_BUILD
  #include "esp_heap_caps.h"
#endif

static constexpr char const * TAG = "WakeSnapshot";

WakeSnapshot WakeSnapshot::singleton;

WakeSnapshot::WakeSnapshot()
  : fb_cache_(nullptr),
    fb_cache_size_(0),
    captured_(false),
    cached_header_{}
{
}

WakeSnapshot::~WakeSnapshot()
{
  if (fb_cache_ != nullptr) {
    free(fb_cache_);
    fb_cache_ = nullptr;
  }
}

bool
WakeSnapshot::capture(uint32_t book_id,
                      const PageLocs::PageId & page_id,
                      uint32_t format_hash)
{
  uint8_t * fb = screen.get_panel_framebuffer();
  size_t    sz = screen.get_panel_framebuffer_size();
  if ((fb == nullptr) || (sz == 0)) return false;

  std::scoped_lock guard(mutex_);

  if ((fb_cache_ == nullptr) || (fb_cache_size_ != sz)) {
    if (fb_cache_ != nullptr) {
      // Framebuffer geometry changed mid-session (orientation flip,
      // resolution change). The old buffer can't carry forward; an
      // old-size capture would fail the geometry check on restore
      // regardless. Drop and re-alloc at the new size.
      free(fb_cache_);
      fb_cache_      = nullptr;
      fb_cache_size_ = 0;
      captured_      = false;
    }
    #if EPUB_INKPLATE_BUILD
      // PSRAM is plentiful (8 MB on PaperS3) and the framebuffer is
      // ~256 KB, so we keep the cache resident for the whole session.
      // Re-allocating on every page paint would be repeated PSRAM
      // heap churn for no functional gain; the alternative (stack
      // buffer per capture) won't fit on the rendering pthread's
      // stack.
      fb_cache_ = (uint8_t *) heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    #else
      fb_cache_ = (uint8_t *) malloc(sz);
    #endif
    if (fb_cache_ == nullptr) {
      LOG_E("PSRAM alloc for snapshot cache failed (%u bytes)", (unsigned)sz);
      // Invariant: fb_cache_ == nullptr => fb_cache_size_ == 0 and
      // captured_ == false. Hold the line so a stale captured_=true
      // can't survive a failed re-alloc and trip persist() into
      // dereferencing nullptr.
      fb_cache_size_ = 0;
      captured_      = false;
      return false;
    }
    fb_cache_size_ = sz;
  }

  memcpy(fb_cache_, fb, sz);

  cached_header_ = {};
  cached_header_.magic         = MAGIC;
  cached_header_.version       = VERSION;
  cached_header_.book_id       = book_id;
  cached_header_.itemref_index = page_id.itemref_index;
  cached_header_.page_offset   = page_id.offset;
  cached_header_.format_hash   = format_hash;
  cached_header_.fb_width      = (int16_t) Screen::get_width();
  cached_header_.fb_height     = (int16_t) Screen::get_height();
  cached_header_.fb_bpp        = 4;
  cached_header_.fb_size       = (uint32_t) sz;
  cached_header_.fb_crc        = crc32(fb_cache_, sz);

  captured_ = true;
  return true;
}

bool
WakeSnapshot::persist()
{
  std::scoped_lock guard(mutex_);

  if (!captured_ || (fb_cache_ == nullptr)) {
    LOG_D("persist: nothing captured, skipping");
    return false;
  }

  FILE * f = fopen(SNAPSHOT_PATH, "wb");
  if (f == nullptr) {
    LOG_E("persist: fopen(%s) failed (errno=%d)", SNAPSHOT_PATH, errno);
    return false;
  }

  bool ok = true;
  if (fwrite(&cached_header_, sizeof(cached_header_), 1, f) != 1) {
    LOG_E("persist: header write failed");
    ok = false;
  }
  if (ok && (fwrite(fb_cache_, 1, fb_cache_size_, f) != fb_cache_size_)) {
    LOG_E("persist: framebuffer write failed");
    ok = false;
  }
  fclose(f);

  if (!ok) {
    // Half-written file would fail magic/CRC on read but explicit
    // unlink is cleaner — keeps a known-bad file from accumulating
    // SD bytes across repeated failed sleeps.
    remove(SNAPSHOT_PATH);
    return false;
  }

  LOG_I("persist: %u-byte snapshot written for book_id=%u page=(%d,%d)",
        (unsigned)(sizeof(cached_header_) + fb_cache_size_),
        (unsigned)cached_header_.book_id,
        (int)cached_header_.itemref_index,
        (int)cached_header_.page_offset);
  return true;
}

bool
WakeSnapshot::restore_to_panel(uint32_t * book_id_out,
                               PageLocs::PageId * page_id_out,
                               uint32_t * format_hash_out)
{
  uint8_t * fb = screen.get_panel_framebuffer();
  size_t    sz = screen.get_panel_framebuffer_size();
  if ((fb == nullptr) || (sz == 0)) {
    LOG_D("restore: screen framebuffer not available");
    return false;
  }

  FILE * f = fopen(SNAPSHOT_PATH, "rb");
  if (f == nullptr) {
    LOG_D("restore: no snapshot file (%s)", SNAPSHOT_PATH);
    return false;
  }

  Header hdr;
  if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
    LOG_W("restore: header read failed");
    fclose(f);
    return false;
  }

  if (hdr.magic != MAGIC) {
    LOG_W("restore: magic mismatch (0x%08x)", (unsigned)hdr.magic);
    fclose(f);
    return false;
  }
  if (hdr.version != VERSION) {
    LOG_W("restore: version mismatch (got %u, expected %u)",
          (unsigned)hdr.version, (unsigned)VERSION);
    fclose(f);
    return false;
  }
  if ((size_t)hdr.fb_size != sz) {
    LOG_W("restore: fb_size mismatch (got %u, expected %u)",
          (unsigned)hdr.fb_size, (unsigned)sz);
    fclose(f);
    return false;
  }
  if ((hdr.fb_width != (int16_t)Screen::get_width()) ||
      (hdr.fb_height != (int16_t)Screen::get_height())) {
    LOG_W("restore: geometry mismatch (snap %dx%d vs panel %dx%d)",
          (int)hdr.fb_width, (int)hdr.fb_height,
          (int)Screen::get_width(), (int)Screen::get_height());
    fclose(f);
    return false;
  }

  // Read into a temporary PSRAM buffer first, validate CRC, *then*
  // commit to the panel framebuffer. Reading directly into the panel
  // fb and clearing on CRC fail (the obvious approach) means a
  // corrupt file pre-stomps the panel state for nothing — the
  // wallpaper is already there from deep-sleep retention and the
  // existing fullclear+GC16 path handles ghosting whether or not the
  // restore succeeds. Keeping the panel fb untouched on failure
  // cedes control cleanly to the normal boot rendering.
  #if EPUB_INKPLATE_BUILD
    uint8_t * tmp = (uint8_t *) heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
  #else
    uint8_t * tmp = (uint8_t *) malloc(sz);
  #endif
  if (tmp == nullptr) {
    LOG_W("restore: PSRAM alloc for read buffer failed (%u bytes)",
          (unsigned)sz);
    fclose(f);
    return false;
  }
  size_t rd = fread(tmp, 1, sz, f);
  fclose(f);
  if (rd != sz) {
    LOG_W("restore: body read failed (got %u of %u)",
          (unsigned)rd, (unsigned)sz);
    free(tmp);
    return false;
  }

  uint32_t crc = crc32(tmp, sz);
  if (crc != hdr.fb_crc) {
    LOG_W("restore: CRC mismatch (0x%08x != 0x%08x), leaving panel "
          "framebuffer untouched",
          (unsigned)crc, (unsigned)hdr.fb_crc);
    free(tmp);
    return false;
  }

  memcpy(fb, tmp, sz);
  free(tmp);

  if (book_id_out)     *book_id_out     = hdr.book_id;
  if (page_id_out)     *page_id_out     = PageLocs::PageId(hdr.itemref_index, hdr.page_offset);
  if (format_hash_out) *format_hash_out = hdr.format_hash;

  // Trigger the panel paint. screen.setup armed s_force_full and
  // s_warm_wake_clear_pending on the warm-wake path; this single
  // call drives the fullclear + GC16 paint in one shot. We paint
  // here (inside the model) rather than at the call site because
  // the force_full / clear_pending arming is a transactional
  // sequence with the framebuffer load — splitting them would let
  // a future caller forget the update() and leave the wallpaper on
  // screen forever.
  screen.update(Screen::UpdateMode::FULL);

  LOG_I("restore: snapshot painted for book_id=%u page=(%d,%d)",
        (unsigned)hdr.book_id,
        (int)hdr.itemref_index,
        (int)hdr.page_offset);
  return true;
}

void
WakeSnapshot::invalidate()
{
  std::scoped_lock guard(mutex_);
  captured_ = false;
  cached_header_ = {};
  if (remove(SNAPSHOT_PATH) == 0) {
    LOG_D("invalidate: snapshot file removed");
  }
  // ENOENT is the common case (no file to remove); not worth a log.
}

uint32_t
WakeSnapshot::format_params_hash(const void * format_params, size_t size)
{
  // FNV-1a 32-bit. Stable, header-only, no dependency. The format
  // params struct is 7 bytes packed (BookFormatParams) so collisions
  // are not a concern in practice.
  uint32_t h = 0x811c9dc5u;
  const uint8_t * p = (const uint8_t *) format_params;
  for (size_t i = 0; i < size; ++i) {
    h ^= p[i];
    h *= 0x01000193u;
  }
  return h;
}

uint32_t
WakeSnapshot::crc32(const uint8_t * data, size_t len)
{
  // Standard CRC-32 (IEEE 802.3 / zlib polynomial 0xEDB88320),
  // table-less form. ~256 KB at PSRAM speed runs in a few ms — not
  // worth the 1 KB table. Used only at capture and restore, both
  // off the page-turn path.
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    c ^= data[i];
    for (int b = 0; b < 8; ++b) {
      // Fully-unsigned mask: 0u - (c & 1u) is 0xFFFFFFFF when bit 0
      // is set, 0 otherwise. Equivalent to the more common
      // -(int32_t)(c & 1) idiom but without relying on
      // two's-complement signed semantics that are technically UB
      // (-INT32_MIN). Same code-gen on every toolchain we care about.
      uint32_t mask = 0u - (c & 1u);
      c = (c >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~c;
}
