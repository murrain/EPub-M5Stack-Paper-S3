// Copyright (c) 2026 Patrick Smith
//
// MIT License. Look at file licenses.txt for details.

#define __WAKE_SNAPSHOT__ 1
#include "models/wake_snapshot.hpp"

#include "screen.hpp"
#include "logging.hpp"
#include "esp.hpp"
#include "models/page_cache.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <sys/types.h>

#if EPUB_INKPLATE_BUILD
  #include "esp_heap_caps.h"
#endif

static constexpr char const * TAG = "WakeSnapshot";

WakeSnapshot WakeSnapshot::singleton;

WakeSnapshot::WakeSnapshot()
  : fb_cache_(nullptr),
    fb_cache_size_(0),
    captured_(false),
    last_capture_ms_(0),
    cached_header_{},
    cached_primary_meta_{}
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

  // Throttle. capture() costs a 256 KB PSRAM memcpy + a CRC over
  // the same buffer; on a fast forward sweep BookController::show_
  // and_capture would call us once per page paint, burning ~10-15
  // ms of bandwidth that the user doesn't notice individually but
  // adds up to lost responsiveness during multi-page swipes.
  //
  // Skipping is safe for sleep persistence: fb_cache_ already
  // holds a recent prior page, persisted as the primary entry on
  // the next deep-sleep entry. The user's typical sleep flow is
  // "land on a page, pause to read for several seconds, open menu,
  // tap Power Off" — which gives the throttle plenty of idle time
  // to let a fresh capture through before sleep fires. PageCache
  // entries persisted alongside cover the neighborhood anyway.
  // ESP::millis() is uint32_t wall-clock-from-boot — wraps at
  // ~49.7 days. The unsigned subtraction is overflow-safe (modular
  // arithmetic), but on the wrap boundary the delta computes as a
  // large value rather than the true ~zero, which would let a
  // capture through despite the throttle saying "old enough." That
  // false negative is harmless — at worst we do one extra capture
  // every ~49 days. The reverse case (false positive blocking a
  // legitimate capture) is impossible because a small delta on
  // either side of wrap stays small under modular arithmetic.
  uint32_t now_ms = (uint32_t) ESP::millis();
  if (captured_ && ((now_ms - last_capture_ms_) < MIN_CAPTURE_INTERVAL_MS)) {
    return false;
  }

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

  // v2: shared metadata in cached_header_, per-page meta in
  // cached_primary_meta_. page_count is set at persist() time
  // (= 1 + however many entries PageCache reports complete).
  cached_header_              = {};
  cached_header_.magic        = MAGIC;
  cached_header_.version      = VERSION;
  cached_header_.book_id      = book_id;
  cached_header_.format_hash  = format_hash;
  cached_header_.fb_width     = (int16_t) Screen::get_width();
  cached_header_.fb_height    = (int16_t) Screen::get_height();
  cached_header_.fb_bpp       = 4;
  cached_header_.fb_size      = (uint32_t) sz;
  // page_count is set by persist() once the live PageCache entry
  // count is known. Leaving it 0 here means an accidental persist
  // without the proper enumerate-and-set step would produce a
  // self-rejecting file (page_count==0 fails the restore check).

  cached_primary_meta_               = {};
  cached_primary_meta_.itemref_index = page_id.itemref_index;
  cached_primary_meta_.page_offset   = page_id.offset;
  cached_primary_meta_.fb_crc        = crc32(fb_cache_, sz);

  captured_        = true;
  last_capture_ms_ = now_ms;
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

  // Phase C: collect PageCache complete entries to persist
  // alongside the primary captured page. enumerate_complete
  // returns framebuffer pointers under a released cache_mutex_,
  // so the lifetime invariant the caller must honor is
  // "no other thread mutates the cache between enumerate and
  // the fwrite of the last entry's body."
  //
  // That invariant holds here because going_to_deep_sleep runs
  // on mainTask, mainTask is the only thread that mutates the
  // cache (all the lifecycle hooks — stop, invalidate_all,
  // start — fire from controller code on this same task), and
  // the pre-paint thread only writes to its own currently-
  // assigned slot (under book_viewer.get_mutex which mainTask
  // doesn't hold during persist, but pre-paint never touches
  // ANOTHER slot than the one it's pre-painting). The cache
  // ordering in app_controller.cpp::going_to_deep_sleep —
  // persist BEFORE page_cache.stop — is what makes this
  // capture meaningful at all.
  PageCache::CompleteEntry extra[PageCache::SLOT_COUNT];
  size_t extra_count = page_cache.enumerate_complete(extra, PageCache::SLOT_COUNT);

  // Filter out the primary page from extras so we don't write the
  // same bitmap twice. The cache may or may not have the current
  // page depending on whether pre-paint completed for it.
  size_t kept = 0;
  for (size_t i = 0; i < extra_count; ++i) {
    if ((extra[i].page_id.itemref_index == cached_primary_meta_.itemref_index) &&
        (extra[i].page_id.offset        == cached_primary_meta_.page_offset)) {
      continue;
    }
    if (i != kept) extra[kept] = extra[i];
    ++kept;
  }
  extra_count = kept;

  cached_header_.page_count = (uint16_t)(1 + extra_count);

  FILE * f = fopen(SNAPSHOT_PATH, "wb");
  if (f == nullptr) {
    LOG_E("persist: fopen(%s) failed (errno=%d)", SNAPSHOT_PATH, errno);
    return false;
  }

  bool ok = true;
  // 1. Header
  if (fwrite(&cached_header_, sizeof(cached_header_), 1, f) != 1) {
    LOG_E("persist: header write failed");
    ok = false;
  }

  // 2. PageEntryMeta array — primary first, then extras in cache
  //    enumeration order (slot index, not navigation order; the
  //    PageId itself is what hydrate keys off).
  if (ok && (fwrite(&cached_primary_meta_, sizeof(cached_primary_meta_), 1, f) != 1)) {
    LOG_E("persist: primary meta write failed");
    ok = false;
  }
  for (size_t i = 0; ok && (i < extra_count); ++i) {
    PageEntryMeta meta = {};
    meta.itemref_index = extra[i].page_id.itemref_index;
    meta.page_offset   = extra[i].page_id.offset;
    meta.fb_crc        = crc32(extra[i].framebuffer, extra[i].fb_size);
    if (fwrite(&meta, sizeof(meta), 1, f) != 1) {
      LOG_E("persist: extra meta %u write failed", (unsigned) i);
      ok = false;
    }
  }

  // 3. Framebuffers — primary first, then extras in matching order.
  if (ok && (fwrite(fb_cache_, 1, fb_cache_size_, f) != fb_cache_size_)) {
    LOG_E("persist: primary framebuffer write failed");
    ok = false;
  }
  for (size_t i = 0; ok && (i < extra_count); ++i) {
    if (fwrite(extra[i].framebuffer, 1, extra[i].fb_size, f) != extra[i].fb_size) {
      LOG_E("persist: extra framebuffer %u write failed", (unsigned) i);
      ok = false;
    }
  }
  fclose(f);

  if (!ok) {
    // Half-written file would fail magic/CRC on read but explicit
    // unlink is cleaner — keeps a known-bad file from accumulating
    // SD bytes across repeated failed sleeps.
    remove(SNAPSHOT_PATH);
    return false;
  }

  size_t total_bytes = sizeof(cached_header_)
                     + (1 + extra_count) * sizeof(PageEntryMeta)
                     + (1 + extra_count) * fb_cache_size_;
  LOG_I("persist: %u-byte v%u snapshot, %u pages, book_id=%u primary=(%d,%d)",
        (unsigned) total_bytes,
        (unsigned) VERSION,
        (unsigned)(1 + extra_count),
        (unsigned) cached_header_.book_id,
        (int) cached_primary_meta_.itemref_index,
        (int) cached_primary_meta_.page_offset);
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
  if (hdr.page_count == 0) {
    LOG_W("restore: page_count is zero");
    fclose(f);
    return false;
  }

  // Phase C: read the primary entry's PageEntryMeta to get its
  // PageId + CRC. We deliberately ignore meta entries 1..N-1 here
  // — those are for the cache-hydration pass run later from
  // BookController::open_book_file via hydrate_page_cache().
  PageEntryMeta primary_meta;
  if (fread(&primary_meta, sizeof(primary_meta), 1, f) != 1) {
    LOG_W("restore: primary meta read failed");
    fclose(f);
    return false;
  }

  // Skip past the rest of the meta array to land at the start of
  // the primary framebuffer (entry 0 of the body block).
  long fb_array_offset = (long) sizeof(Header)
                       + (long) hdr.page_count * (long) sizeof(PageEntryMeta);
  if (fseek(f, fb_array_offset, SEEK_SET) != 0) {
    LOG_W("restore: fseek to body failed");
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
  if (crc != primary_meta.fb_crc) {
    LOG_W("restore: primary CRC mismatch (0x%08x != 0x%08x), leaving "
          "panel framebuffer untouched",
          (unsigned)crc, (unsigned)primary_meta.fb_crc);
    free(tmp);
    return false;
  }

  memcpy(fb, tmp, sz);
  free(tmp);

  if (book_id_out)     *book_id_out     = hdr.book_id;
  if (page_id_out)     *page_id_out     = PageLocs::PageId(primary_meta.itemref_index,
                                                           primary_meta.page_offset);
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

  LOG_I("restore: primary painted for book_id=%u page=(%d,%d), "
        "%u extra pages available for hydration",
        (unsigned) hdr.book_id,
        (int) primary_meta.itemref_index,
        (int) primary_meta.page_offset,
        (unsigned) (hdr.page_count - 1));
  return true;
}

size_t
WakeSnapshot::hydrate_page_cache(uint32_t expected_book_id,
                                 uint32_t expected_format_hash)
{
  // Re-open the snapshot file (it was closed at the end of
  // restore_to_panel) and scan the entries past the primary,
  // injecting each into PageCache. PageCache must already be
  // started by the caller — typically from BookController::open_
  // book_file just after the page_cache.start() call but BEFORE
  // the first show_and_capture fires (so this hydration's slots
  // take precedence over fresh pre-paint requests for the same
  // pages).
  FILE * f = fopen(SNAPSHOT_PATH, "rb");
  if (f == nullptr) {
    LOG_D("hydrate: no snapshot file");
    return 0;
  }

  Header hdr;
  if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
    fclose(f);
    return 0;
  }
  if ((hdr.magic != MAGIC) || (hdr.version != VERSION) ||
      (hdr.book_id != expected_book_id) ||
      (hdr.format_hash != expected_format_hash)) {
    LOG_D("hydrate: header / book_id / format_hash mismatch — skipping");
    fclose(f);
    return 0;
  }
  if ((size_t) hdr.fb_size != screen.get_panel_framebuffer_size()) {
    LOG_W("hydrate: fb_size mismatch (got %u, expected %u)",
          (unsigned) hdr.fb_size,
          (unsigned) screen.get_panel_framebuffer_size());
    fclose(f);
    return 0;
  }
  if (hdr.page_count <= 1) {
    LOG_D("hydrate: only primary page in snapshot, nothing to inject");
    fclose(f);
    return 0;
  }
  // Clamp against the maximum we could possibly hydrate. Without
  // this, a corrupted file claiming page_count=65535 would cause
  // a 768 KB malloc, a multi-MB seek loop, and 64-bit overflow
  // in the offset arithmetic on 32-bit ESP-IDF (long is 32 bits
  // here). Cap at SLOT_COUNT + 1 (primary + all cache slots) —
  // anything above that can't possibly have been written by a
  // correct persist().
  const uint16_t max_pages = (uint16_t)(PageCache::SLOT_COUNT + 1);
  if (hdr.page_count > max_pages) {
    LOG_W("hydrate: page_count %u exceeds cap %u; corrupt file?",
          (unsigned) hdr.page_count, (unsigned) max_pages);
    fclose(f);
    return 0;
  }

  size_t injected = 0;
  size_t fb_size  = (size_t) hdr.fb_size;

  // Allocate a single PSRAM scratch buffer that we reuse for each
  // entry's body read + CRC + inject. Avoids N allocations and N
  // frees during what should be a fast warm-wake step.
  #if EPUB_INKPLATE_BUILD
    uint8_t * scratch = (uint8_t *) heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM);
  #else
    uint8_t * scratch = (uint8_t *) malloc(fb_size);
  #endif
  if (scratch == nullptr) {
    LOG_W("hydrate: PSRAM alloc for scratch buffer failed");
    fclose(f);
    return 0;
  }

  // Read all PageEntryMeta entries up-front. Stack-allocated since
  // page_count is now bounded to SLOT_COUNT + 1 (8) — that's only
  // 96 bytes, no need for malloc + failure path.
  PageEntryMeta metas[PageCache::SLOT_COUNT + 1];
  if (fread(metas, sizeof(PageEntryMeta), hdr.page_count, f) != hdr.page_count) {
    LOG_W("hydrate: meta array read failed");
    free(scratch);
    fclose(f);
    return 0;
  }

  // Iterate entries 1..page_count-1 (skip primary, already on
  // panel via restore_to_panel). For each: seek to its framebuffer
  // offset, read into scratch, CRC, inject.
  // off_t is 64-bit on most platforms; safe for multi-MB offsets.
  off_t fb_array_offset = (off_t) sizeof(Header)
                        + (off_t) hdr.page_count * (off_t) sizeof(PageEntryMeta);
  for (uint16_t i = 1; i < hdr.page_count; ++i) {
    off_t entry_offset = fb_array_offset + (off_t) i * (off_t) fb_size;
    if (fseek(f, (long) entry_offset, SEEK_SET) != 0) {
      LOG_W("hydrate: fseek to entry %u failed", (unsigned) i);
      break;
    }
    if (fread(scratch, 1, fb_size, f) != fb_size) {
      LOG_W("hydrate: entry %u read failed", (unsigned) i);
      break;
    }
    uint32_t crc = crc32(scratch, fb_size);
    if (crc != metas[i].fb_crc) {
      LOG_W("hydrate: entry %u CRC mismatch — skipping", (unsigned) i);
      continue;
    }
    PageLocs::PageId pid(metas[i].itemref_index, metas[i].page_offset);
    if (page_cache.inject_entry(pid, hdr.format_hash, scratch, fb_size)) {
      ++injected;
    } else {
      LOG_D("hydrate: inject_entry rejected page (%d,%d) — slot full?",
            (int) metas[i].itemref_index,
            (int) metas[i].page_offset);
    }
  }

  free(scratch);
  fclose(f);

  LOG_I("hydrate: %u extra page(s) injected into PageCache", (unsigned) injected);
  return injected;
}

void
WakeSnapshot::invalidate()
{
  std::scoped_lock guard(mutex_);
  captured_            = false;
  // Symmetric reset: keeps "freshly invalidated" indistinguishable
  // from "freshly constructed" so the throttle gate doesn't carry
  // stale timestamp state across an invalidate. Defense-in-depth
  // against a future change that drops the captured_ guard on the
  // throttle check.
  last_capture_ms_     = 0;
  cached_header_       = {};
  cached_primary_meta_ = {};
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
