// Copyright (c) 2026 Patrick Smith
//
// MIT License. Look at file licenses.txt for details.

#pragma once
#include "global.hpp"

#include "models/page_locs.hpp"
#include "models/wake_snapshot.hpp"

#include <cstdint>
#include <cstddef>
#include <mutex>

// PageCache: PSRAM-backed map of pre-rendered page framebuffers around
// the user's current reading position. Background pthread renders
// pages N-2..N+2 (the "±N residency" set) into PSRAM scratch buffers
// while the user reads page N, so that swipes hit a ready bitmap and
// only pay the ~120 ms MODE_DU waveform — no layout, no glyph metrics,
// no DOM walk on the user-perceived path.
//
// Lifecycle model
// ---------------
//   - Module is PaperS3-only (relies on Screen::ScopedRenderTarget +
//     get_panel_framebuffer, both of which return null/no-op on
//     Inkplate/Linux). On non-PaperS3 boards every public method is
//     a no-op so callers don't need #ifdef walls.
//   - start() is called when a book opens (BookController::open_book_
//     file). Allocates one 1.8 MB PSRAM slab sliced into 7 entry
//     buffers; on alloc failure the cache disables itself and falls
//     back to "foreground render only" (no crash).
//   - request_residency() takes the desired ±N PageId set; the
//     module evicts entries outside the set and enqueues pre-paint
//     jobs for missing entries.
//   - get() returns a const pointer to a cached bitmap if the entry
//     is complete AND its format-params hash matches the caller's
//     current hash; otherwise returns nullptr.
//   - invalidate_all() drops every entry + cancels in-flight jobs.
//     Called on format-param edits, orientation change, and book
//     switch.
//   - stop() is called on book close (BooksDirController::enter).
//     STOP/STOPPED handshake guarantees the pre-paint task has
//     exited any ScopedRenderTarget AND released book_viewer's
//     mutex before the slab is freed — same shape as the retriever
//     handshake documented at the top of page_locs.cpp.
//
// Threading model
// ---------------
// Three contenders for book_viewer.get_mutex():
//   1. mainTask — show_page (foreground render), input handling
//   2. retrieverTask — build_page_locs (page-location computation)
//   3. pre-paint task (NEW) — page rendering into off-screen buffer
//
// Acquisition order: the pre-paint task acquires book_viewer.get_
// mutex() for the ENTIRE duration of build_pages_recurse, mirroring
// the retriever's pattern. The PageCache's own mutex (cache_mutex_)
// protects only the entry-table state (which entry holds which
// PageId, complete flag, format hash) and is NEVER held across
// book_viewer.get_mutex() acquisition. Holding both at once would
// invert the lock order and risk deadlock against the retriever.
//
// On-disk persistence (Phase C)
// -----------------------------
// PageCache itself does not touch SD. Phase C extends WakeSnapshot
// to multi-page format and consumes PageCache's enumerate_complete()
// at deep-sleep entry to write all valid entries. On warm wake,
// hydrate_from() repopulates entries from the persisted snapshot.

class PageCache
{
  public:
    // Snapshot of one cache entry returned by enumerate_complete()
    // for Phase C persistence. The framebuffer pointer is valid
    // only until the next mutating cache operation; caller must
    // copy or persist before releasing the cache mutex.
    struct CompleteEntry {
      PageLocs::PageId  page_id;
      uint32_t          format_hash;
      const uint8_t *   framebuffer;
      size_t            fb_size;
    };

    // Maximum simultaneously-cached pages. Sized for "current ± 3"
    // around a reader's position. PSRAM cost: SLOT_COUNT *
    // get_panel_framebuffer_size() = 7 * ~256 KB ≈ 1.8 MB.
    static constexpr size_t SLOT_COUNT = 7;

    PageCache();
    ~PageCache();

    // Initialize the cache for a freshly-opened book. Allocates the
    // PSRAM slab, spawns the pre-paint pthread, and resets the
    // entry table. Idempotent: a second start() call without an
    // intervening stop() is a no-op + warning.
    //
    // Returns false if PSRAM allocation fails — caller should treat
    // this as "cache disabled," NOT as a fatal book-open failure.
    // Subsequent get() calls will return nullptr; foreground render
    // continues to work via the existing book_viewer.show_page path.
    bool start();

    // Stop the pre-paint pthread (STOP/STOPPED handshake) and free
    // the PSRAM slab. Called on book-close (BooksDirController::
    // enter) BEFORE page_locs/epub teardown — the pre-paint task
    // dereferences PageLocs::item_info during render, and freeing
    // it before the task is confirmed-stopped would UAF.
    void stop();

    // Drop all cached entries + cancel in-flight pre-paint jobs.
    // Pre-paint pthread keeps running (ready for re-population);
    // only the entry contents are invalidated. Called on format
    // edit, orientation change, and as part of book switch. Cheap.
    void invalidate_all();

    // Quiesce the pre-paint pthread without freeing the slab.
    // Called on every transition out of BOOK (BookController::
    // leave) so foreground code paths that paint via screen.draw_*
    // — menu_viewer, msg_viewer, battery_viewer, sleep_screen,
    // wallpaper, usb_msc_viewer — never race the pthread's
    // ScopedRenderTarget guard. Without this pause, those paths'
    // screen.update() calls trip Phase A's "active==panel"
    // assertion when pre-paint is mid-render and abort() the
    // device. resume() restores normal pre-paint operation when
    // BookController::enter runs again.
    //
    // Same shape as stop() — STOP-sentinel + ACK handshake — but
    // does NOT free the slab or join the pthread, so re-entry to
    // BOOK doesn't pay the alloc + thread-spawn cost on every
    // PARAM/TOC trip.
    void pause();
    void resume();

    // Request that the cache hold exactly the given set of pages.
    // Entries outside the set are evicted (slots become available);
    // entries inside the set that aren't already cached are queued
    // for pre-paint. Caller passes the current format hash; jobs
    // capture it at enqueue time so format-change races detected
    // at render time can drop stale jobs. Caller (BookController)
    // computes the ± N set from current_page_id via page_locs.get_
    // next/prev_page_id.
    //
    // PRECONDITION: any entries already in the cache must have a
    // format_hash equal to current_format_hash. This is enforced by
    // every code path that changes format params calling invalidate_
    // all FIRST (BookParamController format-edit + revert hooks).
    // If a future caller mutates format params without invalidate,
    // get() will correctly return nullptr for the stale slot but
    // it will also occupy a slot until evicted by residency drift,
    // wasting one cache entry until the user navigates away.
    void request_residency(const PageLocs::PageId * page_ids,
                           size_t n,
                           uint32_t current_format_hash);

    // Lookup. Returns the cached framebuffer pointer if there's a
    // complete entry for page_id whose format hash matches the
    // current hash; otherwise returns nullptr.
    //
    // POINTER LIFETIME CONTRACT: the returned pointer is valid
    // only as long as the caller doesn't yield and no OTHER thread
    // calls a cache-mutating method (request_residency, invalidate
    // _all, stop). In practice this means: call get → memcpy the
    // bytes you need → use the copy. Today the only caller is
    // BookController::show_and_capture on mainTask, and the only
    // mutating caller (besides mainTask) is the pre-paint thread
    // — but pre-paint serializes with show_and_capture via book_
    // viewer.get_mutex(), so the memcpy under that mutex is safe.
    //
    // If a future caller invokes invalidate_all from a non-mainTask
    // context (e.g. a power-event handler), the BV-mutex won't help
    // and the show_and_capture memcpy could race the slot becoming
    // !in_use. At that point this API should be redesigned to take
    // a destination buffer and copy under cache_mutex_; see code-
    // quality review on Phase B.
    const uint8_t * get(const PageLocs::PageId & page_id,
                        uint32_t current_format_hash);

    // For Phase C warm-wake hydration: copy a pre-rendered page
    // bitmap (read from the multi-page WakeSnapshot file) directly
    // into a free cache slot, bypassing the pre-paint pthread.
    // Marks the entry complete so subsequent get() calls hit it.
    // Returns true on success, false if there's no free slot or
    // size mismatch. Caller (WakeSnapshot::hydrate_page_cache)
    // checks return + falls back to "let pre-paint repopulate
    // organically as user navigates."
    bool inject_entry(const PageLocs::PageId & page_id,
                      uint32_t format_hash,
                      const uint8_t * source_fb,
                      size_t source_size);

    // For Phase C: snapshot all complete entries under one
    // cache-mutex acquisition. The caller (WakeSnapshot v2) then
    // serializes them to SD. Output buffer must hold up to
    // SLOT_COUNT entries.
    //
    // POINTER LIFETIME CONTRACT (same as get()): each
    // CompleteEntry::framebuffer is valid only as long as the
    // caller doesn't yield and no other thread mutates the cache.
    // Phase C call site is going_to_deep_sleep, where pre-paint
    // and the foreground render path have both been quiesced via
    // page_cache.stop() — wait, no: persist must happen BEFORE
    // stop, so pre-paint is still alive. The right pattern for
    // Phase C is "enumerate → for each entry, fwrite under the
    // SAME held cache-mutex" — i.e. the caller takes the mutex
    // explicitly via a wrapper API, or this method takes a callback
    // and holds cache_mutex_ across the callback. To be designed
    // when Phase C lands; today this returns valid pointers under
    // an immediately-released mutex, which is a footgun the doc
    // here is explicitly calling out.
    size_t enumerate_complete(CompleteEntry * out, size_t max);

    // Whether start() succeeded. Used by BookController to short-
    // circuit cache lookup on platforms / failure paths where the
    // cache is disabled.
    bool is_active() const { return active_; }

    static PageCache & get_singleton() noexcept { return singleton; }

  private:
    static PageCache singleton;

    // Per-slot state. The framebuffer pointer is fixed for the
    // lifetime of one start()/stop() cycle (slice into the slab);
    // page_id / format_hash / complete change as entries are
    // evicted and re-rendered.
    struct Entry {
      uint8_t * framebuffer;       // slice into slab_, never reseated
      PageLocs::PageId page_id;
      uint32_t format_hash;
      bool in_use;                 // slot is allocated to a page
      bool complete;               // pre-paint finished
    };

    // Pre-paint job. format_hash captured at enqueue time so the
    // task can drop stale jobs on the rendering side without
    // racing the cache mutex.
    struct PrePaintJob {
      PageLocs::PageId page_id;
      uint32_t format_hash;
    };

    // Module state.
    std::mutex cache_mutex_;
    Entry      entries_[SLOT_COUNT];
    uint8_t *  slab_;              // single PSRAM allocation
    size_t     fb_size_;           // bytes per entry framebuffer
    bool       active_;            // start() succeeded, stop() not yet

    // Pre-paint pthread state. Defined entirely in page_cache.cpp;
    // the public surface here only exposes the lifecycle hooks the
    // module owner (BookController) needs.

    // Helpers (no locking — caller holds cache_mutex_).
    int find_entry_for(const PageLocs::PageId & page_id);
    int find_free_slot();
    int find_evictable_slot(const PageLocs::PageId * keep_set,
                            size_t keep_count);
    static bool page_id_in_set(const PageLocs::PageId & page_id,
                               const PageLocs::PageId * set,
                               size_t n);

    // Pre-paint task implementation lives in page_cache.cpp as a
    // lambda closure over `this` and module-static queue handles.
    // The lambda accesses cache_mutex_ and entries_ via the
    // captured `this` pointer; no extra friendship needed.
};

#if defined(__PAGE_CACHE__)
PageCache & page_cache = PageCache::get_singleton();
#else
extern PageCache & page_cache;
#endif
