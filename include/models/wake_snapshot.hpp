// Copyright (c) 2026 Patrick Smith
//
// MIT License. Look at file licenses.txt for details.

#pragma once
#include "global.hpp"

#include "models/page_locs.hpp"

#include <cstdint>
#include <cstddef>
#include <mutex>

// Persists a single screen-framebuffer snapshot — the last book page the
// user was viewing — across deep sleep. On warm wake the snapshot is
// painted onto the panel before the slower boot phases (epub.open_file,
// page_locs.start_new_document, book_viewer.show_page) run, so the user
// sees their content essentially immediately.
//
// Why a snapshot rather than re-rendering on wake: rendering a single
// page from epub source on this hardware takes ~700 ms of layout +
// ~700 ms GC16 waveform on top of multi-second SD-side initialization
// (font cache, books_dir refresh, page_locs.load). A pre-rendered
// snapshot collapses that into one ~700 ms GC16 paint of an already-
// computed bitmap.
//
// Lifecycle:
//   1. capture(): called from book_viewer after every page paint.
//      Cheap memcpy of the framebuffer + metadata into a PSRAM cache.
//   2. persist(): called once on the deep-sleep entry path. Writes the
//      cached buffer to /sdcard/.wake_snapshot.bin.
//   3. restore_to_panel(): called once on warm wake from main, between
//      screen.setup and the rest of the boot. Reads the file into the
//      screen framebuffer and triggers the panel update.
//   4. invalidate(): nukes the file when the snapshot would no longer
//      be visually correct (font-size change, orientation flip,
//      different book opened, etc.).
//
// The on-disk format is single-file (header + framebuffer body) so a
// crash mid-write produces a file that fails the magic/version check
// and is safely ignored on the next wake.
class WakeSnapshot
{
  public:
    static constexpr char const * SNAPSHOT_PATH = "/sdcard/.wake_snapshot.bin";

    // File header. Persisted verbatim ahead of the raw framebuffer
    // bytes. Sized to a multiple of 4 so the framebuffer body lands on
    // an aligned offset.
    struct Header
    {
      uint32_t magic;          ///< MAGIC, sanity check on read.
      uint32_t version;        ///< Bump on any incompatible field change.
      uint32_t book_id;        ///< NVS book id at capture time.
      int16_t  itemref_index;  ///< PageId.itemref_index
      int16_t  reserved0;      ///< padding to 4-byte align next field
      int32_t  page_offset;    ///< PageId.offset
      uint32_t format_hash;    ///< Hash of book_format_params at capture.
      int16_t  fb_width;       ///< Physical landscape width (960 on PaperS3).
      int16_t  fb_height;      ///< Physical landscape height (540).
      int8_t   fb_bpp;         ///< 4 (4-bit packed grayscale)
      int8_t   reserved1[3];
      uint32_t fb_size;        ///< Bytes of framebuffer payload following.
      uint32_t fb_crc;         ///< CRC-32 of the framebuffer payload.
    };
    static constexpr uint32_t MAGIC   = 0xFEEDFACEu;
    static constexpr uint32_t VERSION = 1u;

    WakeSnapshot();
    ~WakeSnapshot();

    // Snapshot the current screen framebuffer + book metadata into the
    // PSRAM cache. Cheap (~10 ms memcpy on the order of 256 KB at
    // PSRAM bandwidth). Caller passes the metadata that identifies
    // what's on screen — we don't try to reverse-engineer it from
    // BookController state to keep the dependency one-way.
    bool capture(uint32_t book_id,
                 const PageLocs::PageId & page_id,
                 uint32_t format_hash);

    // True iff capture() has populated the cache since boot.
    inline bool has_pending_capture() const { return captured_; }

    // Write the cached snapshot to SD. Called from the deep-sleep entry
    // path. Returns false if no capture is pending or the SD write
    // fails. On failure the on-disk file is unlinked so the next wake
    // doesn't restore stale content.
    bool persist();

    // On warm wake, read the snapshot from SD into the screen
    // framebuffer and trigger a full-clear + GC16 paint. Returns true
    // if the panel now shows the cached content. Out-params describe
    // the snapshot so the caller can decide whether to invalidate
    // before the rest of boot runs (e.g. format-param mismatch).
    bool restore_to_panel(uint32_t * book_id_out,
                          PageLocs::PageId * page_id_out,
                          uint32_t * format_hash_out);

    // Discard the on-disk snapshot file AND drop the in-memory
    // capture. Expected callers (none yet wired — Stage 2 work):
    //   - BookParamController, after epub.update_book_format_params()
    //     fires (font size, font index, show-images toggle, etc.)
    //   - OptionController, after orientation change
    //   - BookController::open_book_file when a different book is
    //     opened (book_id mismatch with the persisted snapshot)
    //   - main.cpp warm-wake, when restore_to_panel returns a valid
    //     snapshot whose book_id / format_hash don't match the book
    //     the controller will re-open
    //
    // Until those hooks land the on-disk snapshot can occasionally
    // get a few seconds of "wrong-format flash before the real
    // render replaces it" UX — Stage 1 ships with that as a known
    // limitation rather than half-wired invalidation logic.
    void invalidate();

    // Compute a stable hash over the BookFormatParams bytes the user
    // can change between sessions (font_size, font_index, orientation,
    // resolution, etc.). format_params should point to the struct in
    // memory; size is sizeof(BookFormatParams). Wrapped here so the
    // hash function is shared between capture and restore code paths.
    static uint32_t format_params_hash(const void * format_params, size_t size);

    static WakeSnapshot & get_singleton() noexcept { return singleton; }

  private:
    static WakeSnapshot singleton;

    // Mutual exclusion between capture() (rendering pthread, called
    // from BookController::show_and_capture) and persist() (mainTask,
    // called from app_controller.going_to_deep_sleep). Without this,
    // a sleep-button press mid-memcpy would persist a torn buffer
    // whose CRC matches but whose pixels are half-old / half-new —
    // failing silently on the next wake. invalidate() also takes
    // this mutex so it can't race a partial capture.
    std::mutex mutex_;

    // PSRAM-backed copy of the most recent framebuffer. Allocated on
    // first capture(). 256 KB is a small fraction of the 8 MB PSRAM
    // budget and it stays resident for the session — re-allocating
    // on every capture would mean repeated PSRAM heap churn for no
    // gain.
    uint8_t * fb_cache_;
    size_t    fb_cache_size_;
    bool      captured_;
    Header    cached_header_;

    static uint32_t crc32(const uint8_t * data, size_t len);
};

#if defined(__WAKE_SNAPSHOT__)
WakeSnapshot & wake_snapshot = WakeSnapshot::get_singleton();
#else
extern WakeSnapshot & wake_snapshot;
#endif
