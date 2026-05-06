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

    // File header v2 (Stage 2 Phase C: multi-page). The single-
    // page v1 fields (itemref_index / page_offset / fb_crc) move
    // into a per-entry PageEntryMeta array that follows the
    // header; v2 carries shared metadata (book_id, format_hash,
    // geometry) plus a count of how many entries follow.
    //
    // File layout:
    //    [Header]                 32 bytes
    //    [PageEntryMeta][page_count]   12 * N bytes
    //    [Framebuffer ][page_count]   fb_size * N bytes
    //
    // Entry 0 is ALWAYS the "primary" page — the one to paint on
    // warm wake. Entries 1..N-1 are pre-paint cache snapshots that
    // get hydrated into PageCache after BookController::open_book_
    // file runs. format_hash is shared (the cache invariant
    // enforced upstream by invalidate-on-format-edit) so we don't
    // pay 4 bytes per entry to repeat it.
    //
    // v1 compatibility: a v1 file fails the version check on read
    // and is silently ignored (the warm-wake path falls back to
    // normal book-open rendering). v1 files were short-lived and
    // only contained one page anyway, so the loss is one wake
    // worth of "instant page paint" UX, recovered on the next
    // sleep when the persist writes a fresh v2 file.
    struct Header
    {
      uint32_t magic;          ///< MAGIC, sanity check on read.
      uint32_t version;        ///< Bump on any incompatible field change.
      uint32_t book_id;        ///< NVS book id at capture time.
      uint32_t format_hash;    ///< Shared across all entries.
      int16_t  fb_width;       ///< Physical landscape width (960 on PaperS3).
      int16_t  fb_height;      ///< Physical landscape height (540).
      int8_t   fb_bpp;         ///< 4 (4-bit packed grayscale)
      int8_t   reserved0[3];
      uint32_t fb_size;        ///< Bytes per entry framebuffer.
      uint16_t page_count;     ///< 1 (primary only) up to SLOT_COUNT+1.
      uint16_t reserved1;
    };
    struct PageEntryMeta
    {
      int16_t  itemref_index;
      int16_t  reserved0;      ///< 4-byte align next field
      int32_t  page_offset;
      uint32_t fb_crc;         ///< CRC-32 of this entry's framebuffer.
    };
    static constexpr uint32_t MAGIC   = 0xFEEDFACEu;
    static constexpr uint32_t VERSION = 2u;

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

    // Phase C: after BookController::open_book_file has called
    // page_cache.start() but BEFORE the first show_and_capture
    // fires, read all NON-primary entries from the snapshot file
    // and inject them into PageCache. Each successful inject_entry
    // call gives the user one more page they can swipe to instantly
    // post-wake without waiting for the pre-paint task.
    //
    // expected_book_id and expected_format_hash gate the load:
    // mismatch (e.g. user wakes-then-immediately-opens-different-
    // book) causes hydrate to skip — those bitmaps wouldn't match
    // the current render anyway. Caller passes the values for the
    // book it just opened.
    //
    // Returns the number of entries successfully injected. Zero
    // is a benign result (no file, version mismatch, all entries
    // already in PageCache, etc.) — the user just doesn't get the
    // multi-page wake bonus this session, but the primary page is
    // already painted (via restore_to_panel earlier on mainTask).
    size_t hydrate_page_cache(uint32_t expected_book_id,
                              uint32_t expected_format_hash);

    // Discard the on-disk snapshot file AND drop the in-memory
    // capture. Caller hook status:
    //
    //   WIRED:
    //   - BookParamController::input_event after
    //     epub.update_book_format_params() fires from the params form
    //     (font size, font index, show-images toggle, etc.)
    //   - BookParamController::revert_to_defaults after the equivalent
    //     update_book_format_params() in the revert path
    //
    //   DEFERRED (Stage 2):
    //   - OptionController, after orientation change
    //   - BookController::open_book_file when a different book is
    //     opened (book_id mismatch with the persisted snapshot —
    //     would use the book_id_out from restore_to_panel)
    //   - main.cpp warm-wake, when restore_to_panel returns a valid
    //     snapshot whose book_id / format_hash don't match the book
    //     the controller will re-open
    //
    // Until the deferred hooks land the on-disk snapshot can
    // occasionally show a few seconds of "wrong-content flash before
    // the real render replaces it" UX on cross-book wakes — the
    // wired hooks cover the format-edit case which is the most
    // common source of staleness.
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
    uint8_t *      fb_cache_;
    size_t         fb_cache_size_;
    bool           captured_;
    // v2 split: shared header fields stay in cached_header_; the
    // per-page metadata (itemref/offset/CRC) for the primary page
    // moves to cached_primary_meta_. persist() writes
    // cached_header_ (page_count = 1 + cached pages from
    // PageCache), then cached_primary_meta_, then PageCache's
    // PageEntryMeta entries, then framebuffers.
    Header         cached_header_;
    PageEntryMeta  cached_primary_meta_;

    static uint32_t crc32(const uint8_t * data, size_t len);
};

#if defined(__WAKE_SNAPSHOT__)
WakeSnapshot & wake_snapshot = WakeSnapshot::get_singleton();
#else
extern WakeSnapshot & wake_snapshot;
#endif
