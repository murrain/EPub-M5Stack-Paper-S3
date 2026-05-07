// Copyright (c) 2026 Patrick Smith
//
// MIT License. Look at file licenses.txt for details.

#define __PAGE_CACHE__ 1
#include "models/page_cache.hpp"

#include "logging.hpp"
#include "screen.hpp"
#include "models/config.hpp"
#include "models/epub.hpp"
#include "models/fonts.hpp"
#include "models/page_locs.hpp"
#include "viewers/book_viewer.hpp"
#include "viewers/html_interpreter.hpp"
#include "viewers/page.hpp"
#include "viewers/screen_bottom.hpp"

#include <cstring>
#include <thread>

#if EPUB_INKPLATE_BUILD && defined(BOARD_TYPE_PAPER_S3)
  #include "esp_heap_caps.h"
  #include "esp_pthread.h"
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"
  #include "freertos/queue.h"
#endif

static constexpr char const * TAG = "PageCache";

PageCache PageCache::singleton;

// =====================================================================
// Pre-paint pthread state
//
// Two queues mirror the PageLocs StateTask handshake:
//
//   prepaint_queue  — caller (mainTask via PageCache::request_residency)
//                     posts PrePaintJob entries
//   stop_ack_queue  — pre-paint task posts a single byte after every
//                     job iteration so PageCache::stop() can synchronize
//                     on "task is currently in QUEUE_RECEIVE blocking,
//                     not mid-render"
//
// On stop():
//   1. cache_aborting = true
//   2. enqueue a sentinel STOP_JOB with page_id.itemref_index = -1
//   3. wait on stop_ack_queue for the STOPPED byte
//   4. join the pthread
//
// Same shape as page_locs.cpp's STOP/STOPPED handshake (see the
// canonical comment block above class StateTask there). The pre-
// paint task is GUARANTEED idle (in QUEUE_RECEIVE on prepaint_queue
// or fully exited) by the time stop() returns, so the slab + entries
// can be freed without racing an in-flight paint.
// =====================================================================

#if EPUB_INKPLATE_BUILD && defined(BOARD_TYPE_PAPER_S3)

namespace
{
  // STOP terminates the pthread; used by stop().
  // NUDGE just wakes the pthread, ACKs, and continues looping;
  //   used by pause() to confirm in-flight renders have exited
  //   their ScopedRenderTarget and released book_viewer.get_mutex.
  // Two sentinels because the pthread needs to distinguish
  // "should I exit?" from "should I just ACK and idle?"
  static constexpr int16_t STOP_SENTINEL_ITEMREF  = -1;
  static constexpr int16_t NUDGE_SENTINEL_ITEMREF = -2;

  static QueueHandle_t prepaint_queue = nullptr;
  static QueueHandle_t stop_ack_queue = nullptr;
  static volatile bool cache_aborting = false;
  static std::thread   prepaint_thread;

  // Render one page-shaped slice of the current book into the
  // entry's framebuffer. Caller (the pre-paint task body below)
  // has already locked book_viewer.get_mutex() and verified the
  // job's format hash matches current state. Returns true on
  // successful render, false if the page wasn't rendered for any
  // reason (item couldn't be loaded, page not yet computed by
  // retriever, etc.).
  //
  // The render mirrors BookViewer::build_page_at — same fmt
  // construction, same BookViewerInterp instantiation, same
  // build_pages_recurse flow — except the final paint is
  // paint_to_active_target() (Phase A's no-update variant) so we
  // don't push the off-screen buffer to the panel.
  bool render_page_into(uint8_t * fb,
                        const PageLocs::PageId & page_id);
}

#endif // EPUB_INKPLATE_BUILD && BOARD_TYPE_PAPER_S3

PageCache::PageCache()
  : slab_(nullptr),
    fb_size_(0),
    active_(false)
{
  for (size_t i = 0; i < SLOT_COUNT; ++i) {
    entries_[i] = Entry{nullptr, PageLocs::PageId(0, 0), 0, false, false};
  }
}

PageCache::~PageCache()
{
  if (active_) stop();
}

bool
PageCache::start()
{
#if EPUB_INKPLATE_BUILD && defined(BOARD_TYPE_PAPER_S3)
  std::scoped_lock guard(cache_mutex_);

  if (active_) {
    LOG_W("PageCache::start called twice; ignoring");
    return true;
  }

  size_t single_fb = screen.get_panel_framebuffer_size();
  if (single_fb == 0) {
    LOG_W("PageCache::start: screen not initialized; cache disabled");
    return false;
  }

  // Single contiguous PSRAM allocation — see architecture review M5.
  // 7 separate ~256 KB allocs would invite fragmentation across long
  // sessions; one slab pinned at book-open is deterministic and the
  // failure point is loud + recoverable (cache disables itself, the
  // foreground render path keeps working).
  size_t slab_size = SLOT_COUNT * single_fb;
  slab_ = (uint8_t *) heap_caps_aligned_alloc(
    16, slab_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (slab_ == nullptr) {
    LOG_W("PageCache::start: PSRAM alloc of %u bytes failed; cache disabled",
          (unsigned) slab_size);
    return false;
  }

  fb_size_ = single_fb;
  for (size_t i = 0; i < SLOT_COUNT; ++i) {
    entries_[i].framebuffer = slab_ + (i * single_fb);
    entries_[i].in_use      = false;
    entries_[i].complete    = false;
    entries_[i].format_hash = 0;
  }

  // Queue depth: one outstanding job per slot is plenty. Job posts
  // are non-blocking — caller (BookController) drops on full queue,
  // which only happens if pre-paint is far behind, in which case the
  // residency request will be re-issued on the next navigation
  // anyway.
  if (prepaint_queue == nullptr) {
    prepaint_queue = xQueueCreate(SLOT_COUNT * 2, sizeof(PrePaintJob));
  }
  if (stop_ack_queue == nullptr) {
    stop_ack_queue = xQueueCreate(1, sizeof(uint8_t));
  }
  if ((prepaint_queue == nullptr) || (stop_ack_queue == nullptr)) {
    LOG_E("PageCache::start: queue alloc failed; cache disabled");
    free(slab_);
    slab_ = nullptr;
    return false;
  }

  cache_aborting = false;

  // Match the retriever pattern: PSRAM-backed pthread stack so the
  // 32 KB doesn't eat into DMA-capable internal SRAM. Pinned to
  // core 0 (LCD DMA runs on core 1 unaffected). Priority MATCHED
  // with the retriever (configMAX_PRIORITIES-2) — they share the
  // priority slot rather than pre-paint sitting one below.
  //
  // Rationale: in steady-state reading the retriever is idle in
  // QUEUE_RECEIVE (page_locs is fully computed for the open book),
  // so it can't actually compete for CPU. Promoting pre-paint to
  // the retriever's slot lets pre-paint preempt any other lower-
  // priority work (idle hooks, telemetry, etc.) and keep up with
  // a fast user sweep — especially important now that the cache-
  // hit display path runs without book_viewer.get_mutex and
  // pre-paint can run concurrently with the panel waveform
  // commit.
  //
  // During the brief retriever-active window (book first opens or
  // format-edit recompute), they share core 0 timeslices fairly,
  // and the user is on the splash screen anyway.
  auto cfg = esp_pthread_get_default_config();
  cfg.thread_name      = "prepaintTask";
  cfg.pin_to_core      = 0;
  cfg.stack_size       = 32 * 1024;
  cfg.prio             = configMAX_PRIORITIES - 2;
  cfg.inherit_cfg      = true;
  cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  esp_pthread_set_cfg(&cfg);

  prepaint_thread = std::thread([this]() {
    PrePaintJob job;
    for (;;) {
      if (xQueueReceive(prepaint_queue, &job, portMAX_DELAY) != pdTRUE) {
        continue;
      }
      if (job.page_id.itemref_index == STOP_SENTINEL_ITEMREF) {
        // STOP sentinel — ack and exit pthread.
        uint8_t ack = 1;
        xQueueSend(stop_ack_queue, &ack, 0);
        return;
      }
      if (job.page_id.itemref_index == NUDGE_SENTINEL_ITEMREF) {
        // NUDGE sentinel from pause() — ack and keep looping.
        // We're guaranteed to be OUTSIDE any ScopedRenderTarget
        // when we receive this (we're in QUEUE_RECEIVE), so the
        // pause-caller can safely paint via foreground draw paths
        // after the ACK arrives.
        uint8_t ack = 1;
        xQueueSend(stop_ack_queue, &ack, 0);
        continue;
      }
      if (cache_aborting) continue;

      // Locate the destination slot and read its framebuffer
      // pointer under the cache mutex. We do NOT hold cache_mutex_
      // during render — that would deadlock against any get()
      // caller waiting on the same mutex during a paint, and would
      // invert the lock order vs. book_viewer.get_mutex().
      uint8_t * target_fb = nullptr;
      uint32_t  target_hash_at_enqueue = job.format_hash;
      {
        std::scoped_lock g(cache_mutex_);
        if (!active_) continue;  // stop() raced — drop
        int slot = find_entry_for(job.page_id);
        if (slot < 0) continue;  // residency was dropped — stale
        target_fb = entries_[slot].framebuffer;
      }

      // Re-check format hash against the live state. If the user
      // changed font size between enqueue and now, the hash will
      // diverge and we drop — invalidate_all already nuked the
      // entry table, so finding the slot above means our job is
      // still relevant. But race-free we still verify here.
      uint32_t current_hash = WakeSnapshot::format_params_hash(
        epub.get_book_format_params(),
        sizeof(EPub::BookFormatParams));
      if (current_hash != target_hash_at_enqueue) continue;

      // Pre-render with the book_viewer mutex held — same
      // discipline as retriever's build_page_locs and mainTask's
      // show_page. Concurrent draw_* writes would race the format
      // pool / glyph metrics / page state.
      bool rendered = false;
      {
        std::scoped_lock bv(book_viewer.get_mutex());
        if (cache_aborting) continue;

        rendered = render_page_into(target_fb, job.page_id);
      }

      if (rendered) {
        std::scoped_lock g(cache_mutex_);
        // Mark entry complete only if the slot still holds OUR
        // page — invalidate_all() may have evicted us mid-render.
        int slot = find_entry_for(job.page_id);
        if (slot >= 0) {
          entries_[slot].complete    = true;
          entries_[slot].format_hash = current_hash;
        }
      }
    }
  });

  active_ = true;
  LOG_I("PageCache::start: %u slots, slab=%p fb_size=%u",
        (unsigned) SLOT_COUNT, slab_, (unsigned) single_fb);
  return true;
#else
  return false;
#endif
}

void
PageCache::stop()
{
#if EPUB_INKPLATE_BUILD && defined(BOARD_TYPE_PAPER_S3)
  bool was_active;
  {
    std::scoped_lock guard(cache_mutex_);
    was_active = active_;
    if (active_) {
      active_ = false;
      cache_aborting = true;
    }
  }
  if (!was_active) return;

  // Post STOP sentinel and wait for ACK. STOP/STOPPED handshake
  // — see header comment + canonical doc in page_locs.cpp.
  PrePaintJob stop_job;
  stop_job.page_id     = PageLocs::PageId(STOP_SENTINEL_ITEMREF, 0);
  stop_job.format_hash = 0;
  xQueueSend(prepaint_queue, &stop_job, portMAX_DELAY);

  uint8_t ack = 0;
  xQueueReceive(stop_ack_queue, &ack, portMAX_DELAY);

  if (prepaint_thread.joinable()) prepaint_thread.join();

  // Drain any leftover jobs that arrived between the abort flag
  // and the STOP sentinel — they'd have been dropped by the task,
  // but we want a clean queue for next start().
  PrePaintJob drain;
  while (xQueueReceive(prepaint_queue, &drain, 0) == pdTRUE) {}

  // Free the slab last — task is confirmed idle by the ACK above.
  std::scoped_lock guard(cache_mutex_);
  if (slab_ != nullptr) {
    free(slab_);
    slab_ = nullptr;
  }
  fb_size_ = 0;
  for (size_t i = 0; i < SLOT_COUNT; ++i) {
    entries_[i] = Entry{nullptr, PageLocs::PageId(0, 0), 0, false, false};
  }

  LOG_I("PageCache::stop: pre-paint task joined, slab freed");
#endif
}

void
PageCache::pause()
{
#if EPUB_INKPLATE_BUILD && defined(BOARD_TYPE_PAPER_S3)
  bool need_drain;
  {
    std::scoped_lock guard(cache_mutex_);
    if (!active_) return;
    if (cache_aborting) return;  // already paused
    cache_aborting = true;
    need_drain = true;
  }
  if (!need_drain) return;

  // The pthread is either:
  //   (a) blocked in QUEUE_RECEIVE waiting for a job — fine, our
  //       aborting flag will be checked on the next dequeue, but
  //       there's nothing in flight to wait for. Skip the ACK to
  //       avoid blocking forever.
  //   (b) mid-render holding book_viewer.get_mutex() and the
  //       ScopedRenderTarget — needs to finish or honor the
  //       inner-loop abort check (PageCachePrePaintInterp::
  //       should_abort_inner returns the cache_aborting flag).
  //
  // Probe with a try-acquire on book_viewer's mutex: if we get it
  // immediately, the pthread isn't rendering and we're done. If
  // not, send a NUDGE sentinel and wait for it to come back via
  // the ACK queue.
  if (book_viewer.get_mutex().try_lock()) {
    book_viewer.get_mutex().unlock();
    return;  // case (a): pthread idle, nothing to drain
  }

  // case (b): pthread is rendering. Send a NUDGE sentinel that
  // the lambda recognizes as "ACK and keep looping" — distinct
  // from STOP which exits the pthread. Pre-paint's render also
  // has should_abort_inner() now wired to cache_aborting (set
  // above), so build_pages_recurse exits early; the pthread
  // unwinds, releases the BV mutex + ScopedRenderTarget, gets
  // back to QUEUE_RECEIVE, dequeues the NUDGE, and ACKs.
  // Bounded timeout so a misbehaving render can't deadlock the
  // user's menu open.
  PrePaintJob nudge_job;
  nudge_job.page_id     = PageLocs::PageId(NUDGE_SENTINEL_ITEMREF, 0);
  nudge_job.format_hash = 0;
  // Don't take portMAX_DELAY here — if the queue is full we'd
  // wait for pre-paint to dequeue a real job first, but pre-paint
  // is exactly what we're trying to interrupt. 100 ms is plenty.
  if (xQueueSend(prepaint_queue, &nudge_job, pdMS_TO_TICKS(100)) != pdTRUE) {
    LOG_W("pause: nudge enqueue timed out; pre-paint may finish "
          "current job uninterrupted");
    return;
  }

  uint8_t ack = 0;
  // 2 second cap — should_abort_inner fires every 64 chars so
  // even worst-case page ends within ~50 ms; 2 s is paranoia.
  if (xQueueReceive(stop_ack_queue, &ack, pdMS_TO_TICKS(2000)) != pdTRUE) {
    LOG_W("pause: ACK timed out; pthread may still be rendering");
  }
  // After this returns, the pthread is back at the top of its
  // loop and will see cache_aborting=true on the next iteration,
  // so subsequent jobs will be dropped until resume() clears the
  // flag. The slab and entry table are intact.
#endif
}

void
PageCache::resume()
{
#if EPUB_INKPLATE_BUILD && defined(BOARD_TYPE_PAPER_S3)
  std::scoped_lock guard(cache_mutex_);
  if (!active_) return;
  cache_aborting = false;
#endif
}

void
PageCache::invalidate_all()
{
#if EPUB_INKPLATE_BUILD && defined(BOARD_TYPE_PAPER_S3)
  std::scoped_lock guard(cache_mutex_);
  if (!active_) return;
  for (size_t i = 0; i < SLOT_COUNT; ++i) {
    entries_[i].in_use   = false;
    entries_[i].complete = false;
  }
  // Drain queued jobs — any in-flight job will detect entry
  // missing or hash mismatch and drop on its own.
  PrePaintJob drain;
  while (xQueueReceive(prepaint_queue, &drain, 0) == pdTRUE) {}
  LOG_D("PageCache::invalidate_all: cache cleared");
#endif
}

void
PageCache::request_residency(const PageLocs::PageId * page_ids,
                             size_t n,
                             uint32_t current_format_hash)
{
#if EPUB_INKPLATE_BUILD && defined(BOARD_TYPE_PAPER_S3)
  if ((page_ids == nullptr) || (n == 0)) return;

  std::scoped_lock guard(cache_mutex_);
  if (!active_) return;

  // Evict entries outside the desired set.
  for (size_t i = 0; i < SLOT_COUNT; ++i) {
    if (entries_[i].in_use && !page_id_in_set(entries_[i].page_id, page_ids, n)) {
      entries_[i].in_use   = false;
      entries_[i].complete = false;
    }
  }

  // Allocate slots for desired pages that aren't yet cached, and
  // enqueue pre-paint jobs.
  for (size_t k = 0; k < n; ++k) {
    int existing = find_entry_for(page_ids[k]);
    if (existing >= 0) continue;  // already cached or queued

    int slot = find_free_slot();
    if (slot < 0) {
      // Full set of in-use slots and they're all in the desired
      // set. Should not happen if n <= SLOT_COUNT — caller's set
      // is too large for the cache, just skip the rest.
      LOG_W("request_residency: no free slot for page (%d, %d)",
            (int) page_ids[k].itemref_index,
            (int) page_ids[k].offset);
      break;
    }
    entries_[slot].page_id     = page_ids[k];
    entries_[slot].in_use      = true;
    entries_[slot].complete    = false;
    entries_[slot].format_hash = current_format_hash;

    PrePaintJob job;
    job.page_id     = page_ids[k];
    job.format_hash = current_format_hash;
    if (xQueueSend(prepaint_queue, &job, 0) != pdTRUE) {
      // Queue full — drop. Will be re-requested next navigation.
      LOG_D("request_residency: prepaint queue full, dropping job");
    }
  }
#else
  (void) page_ids; (void) n; (void) current_format_hash;
#endif
}

const uint8_t *
PageCache::get(const PageLocs::PageId & page_id,
               uint32_t current_format_hash)
{
#if EPUB_INKPLATE_BUILD && defined(BOARD_TYPE_PAPER_S3)
  std::scoped_lock guard(cache_mutex_);
  if (!active_) return nullptr;
  int slot = find_entry_for(page_id);
  if (slot < 0) return nullptr;
  if (!entries_[slot].complete) return nullptr;
  if (entries_[slot].format_hash != current_format_hash) return nullptr;
  return entries_[slot].framebuffer;
#else
  (void) page_id; (void) current_format_hash;
  return nullptr;
#endif
}

bool
PageCache::inject_entry(const PageLocs::PageId & page_id,
                        uint32_t format_hash,
                        const uint8_t * source_fb,
                        size_t source_size)
{
#if EPUB_INKPLATE_BUILD && defined(BOARD_TYPE_PAPER_S3)
  if ((source_fb == nullptr) || (source_size == 0)) return false;

  std::scoped_lock guard(cache_mutex_);
  if (!active_) return false;
  if (source_size != fb_size_) {
    LOG_W("inject_entry: size mismatch (got %u, expected %u)",
          (unsigned) source_size, (unsigned) fb_size_);
    return false;
  }

  // If the page is already cached, replace it (newer-from-disk wins
  // over a possibly-stale slot — though in practice both should be
  // the same content given the format_hash precondition).
  int slot = find_entry_for(page_id);
  if (slot < 0) {
    slot = find_free_slot();
    if (slot < 0) {
      LOG_D("inject_entry: no free slot for (%d,%d)",
            (int) page_id.itemref_index, (int) page_id.offset);
      return false;
    }
  }

  std::memcpy(entries_[slot].framebuffer, source_fb, source_size);
  entries_[slot].page_id     = page_id;
  entries_[slot].format_hash = format_hash;
  entries_[slot].in_use      = true;
  entries_[slot].complete    = true;

  return true;
#else
  (void) page_id; (void) format_hash; (void) source_fb; (void) source_size;
  return false;
#endif
}

size_t
PageCache::enumerate_complete(CompleteEntry * out, size_t max)
{
#if EPUB_INKPLATE_BUILD && defined(BOARD_TYPE_PAPER_S3)
  if ((out == nullptr) || (max == 0)) return 0;
  std::scoped_lock guard(cache_mutex_);
  if (!active_) return 0;
  size_t count = 0;
  for (size_t i = 0; i < SLOT_COUNT && count < max; ++i) {
    if (entries_[i].in_use && entries_[i].complete) {
      out[count].page_id     = entries_[i].page_id;
      out[count].format_hash = entries_[i].format_hash;
      out[count].framebuffer = entries_[i].framebuffer;
      out[count].fb_size     = fb_size_;
      ++count;
    }
  }
  return count;
#else
  (void) out; (void) max;
  return 0;
#endif
}

int
PageCache::find_entry_for(const PageLocs::PageId & page_id)
{
  for (size_t i = 0; i < SLOT_COUNT; ++i) {
    if (entries_[i].in_use &&
        entries_[i].page_id.itemref_index == page_id.itemref_index &&
        entries_[i].page_id.offset == page_id.offset) {
      return (int) i;
    }
  }
  return -1;
}

int
PageCache::find_free_slot()
{
  for (size_t i = 0; i < SLOT_COUNT; ++i) {
    if (!entries_[i].in_use) return (int) i;
  }
  return -1;
}

bool
PageCache::page_id_in_set(const PageLocs::PageId & page_id,
                          const PageLocs::PageId * set,
                          size_t n)
{
  for (size_t k = 0; k < n; ++k) {
    if ((set[k].itemref_index == page_id.itemref_index) &&
        (set[k].offset == page_id.offset)) return true;
  }
  return false;
}

// =====================================================================
// Pre-paint render implementation
// =====================================================================

#if EPUB_INKPLATE_BUILD && defined(BOARD_TYPE_PAPER_S3)

namespace
{

// Local subclass mirroring book_viewer.cpp's BookViewerInterp.
// Identical semantics — page_end returns true to signal "stop
// recursion at this page boundary" — but TU-private to that file so
// we have our own copy here. The actual paint call lives in
// render_page_into below, after build_pages_recurse returns,
// matching how book_viewer.cpp::build_page_at structures the work.
class PageCachePrePaintInterp : public HTMLInterpreter
{
  public:
    PageCachePrePaintInterp(Page & p, DOM & d, const EPub::ItemInfo & info)
      : HTMLInterpreter(p, d, Page::ComputeMode::DISPLAY, info) {}

    // Honor pause() / stop() requests mid-render. cache_aborting is
    // set by either pathway; when build_pages_recurse polls this
    // every 64 chars (see html_interpreter.cpp), returning true
    // unwinds the recursion. The pthread then releases book_viewer.
    // get_mutex and the ScopedRenderTarget, gets back to its
    // QUEUE_RECEIVE, and the pause/stop's NUDGE/STOP sentinel
    // arrives with a clean rendering state.
    //
    // Without this, a long page render could hold the panel-active
    // framebuffer pointer at the scratch buffer for hundreds of ms
    // while menu_viewer tries to paint — tripping Phase A's
    // assert(s_active_framebuffer == s_framebuffer) in
    // screen.update() and abort()ing the device. That was the
    // crash report from the first hardware run on the integrated
    // Stage 2 branch.
    bool should_abort_inner() const override { return cache_aborting; }

  protected:
    bool page_end(const Page::Format &) override { return true; }
};

bool render_page_into(uint8_t * fb, const PageLocs::PageId & page_id)
{
  // Note on book_viewer.get_mutex() interaction with page_locs.get_
  // page_info: book_viewer.cpp::build_page_at temporarily UNLOCKS
  // book_viewer.get_mutex() around its get_page_info call (lines
  // 107-110 there) because get_page_info can block on the retriever
  // and holding the BV mutex during that wait would deadlock the
  // retriever (which itself wants the same mutex when it advances
  // page state). Pre-paint runs at lower priority than the retriever
  // and never holds the BV mutex long enough for that deadlock to
  // matter — the worst case is the retriever stalls one item-render
  // behind us. We accept the simpler "hold BV mutex for the whole
  // function" pattern to avoid duplicating the unlock/lock dance,
  // and document the divergence here so a future reviewer doesn't
  // "fix" us into copy-pasting build_page_at's pattern.
  const PageLocs::PageInfo * info = page_locs.get_page_info(page_id);
  if (info == nullptr) {
    LOG_D("prepaint: page (%d,%d) not yet computed; dropping",
          (int) page_id.itemref_index, (int) page_id.offset);
    return false;
  }
  if (info->size <= 0) {
    // Hidden / zero-size page — nothing to render.
    return false;
  }

  // Load the item into epub.current_item_info via the ONE-ARG form
  // of get_item_at_index — same pattern as BookViewer::build_page_
  // at. The two-arg form (used by the retriever's build_page_locs)
  // writes into a SEPARATE PageLocs::item_info member to avoid
  // clobbering current_item_info while the retriever crawls items
  // ahead of the user. We're rendering for visible-near-current
  // display, not for ahead-scan, so the one-arg form is correct
  // here. CAUTION: any future revision that releases book_viewer.
  // get_mutex() mid-render — to yield, to wait on a queue, etc. —
  // MUST switch to the two-arg form, otherwise mainTask's show_
  // page can clobber current_item_info between our load and our
  // build_pages_recurse.
  if (!epub.get_item_at_index(page_id.itemref_index)) {
    LOG_D("prepaint: get_item_at_index(%d) failed", (int) page_id.itemref_index);
    return false;
  }

  // fmt construction now goes through Page::make_body_format,
  // which is the single source of truth for the body-text Page::
  // Format. The previous triple-coupling (this file + book_viewer
  // + page_locs all maintained their own copy in lockstep) caused
  // a silent page_bottom drift between the paginator and renderers
  // (audit 05-code-quality.md finding #1 / fixed in PR1).
  Font * bottom_font = fonts.get(ScreenBottom::FONT);
  int16_t page_bottom =
    bottom_font->get_chars_height(ScreenBottom::FONT_SIZE) + 15;

  int8_t show_title;
  config.get(Config::Ident::SHOW_TITLE, &show_title);

  int16_t page_top              = 0;
  int16_t title_baseline_offset = 0;
  if (show_title != 0) {
    Font * title_font     = fonts.get(book_viewer.TITLE_FONT);
    page_top              = title_font->get_chars_height(book_viewer.TITLE_FONT_SIZE) + 10;
    title_baseline_offset = page_top +
                            title_font->get_descender_height(book_viewer.TITLE_FONT_SIZE);
  }

  int16_t font_idx = fonts.get_index("Fontbase", Fonts::FaceStyle::NORMAL);
  if (font_idx == -1) font_idx = 3;
  int8_t font_size = epub.get_book_format_params()->font_size;

  Page::Format fmt = Page::make_body_format((int16_t) font_idx, font_size,
                                             page_top, page_bottom);

  DOM dom;
  // The Page singleton is shared with book_viewer's foreground
  // render path. book_viewer mutex (held by caller) serializes
  // access, so we have exclusive use of the format_pool / glyph
  // metrics / display_list state for the duration of this render.
  PageCachePrePaintInterp interp(page, dom, epub.get_current_item_info());
  interp.set_limits(page_id.offset,
                    page_id.offset + (int32_t) info->size,
                    epub.get_book_format_params()->show_images == 1);

  // ScopedRenderTarget: all draw_* calls inside paint_to_active_
  // target hit `fb` instead of the panel framebuffer. Guard
  // restores the panel target on scope exit (via destructor) even
  // if build_pages_recurse throws.
  size_t expected_size = screen.get_panel_framebuffer_size();
  Screen::ScopedRenderTarget guard(fb, expected_size);

  // Pre-fill with white (0xFF nibbles for 4bpp grayscale white).
  // Page rendering only writes glyph/image pixels, leaving the
  // background untouched — without this, the cache buffer would
  // show whatever was previously in PSRAM (garbage on first use,
  // stale-page from prior eviction on re-use).
  std::memset(fb, 0xFF, expected_size);

  xml_node node = epub.get_current_item().child("html").child("body");
  if (!node) {
    LOG_D("prepaint: no <body> in page (%d,%d)",
          (int) page_id.itemref_index, (int) page_id.offset);
    return false;
  }

  page.start(fmt);
  Page::Format * new_fmt = interp.duplicate_fmt(fmt);

  if (interp.build_pages_recurse(node, *new_fmt, dom.body, 1)) {
    if (page.some_data_waiting()) page.end_paragraph(fmt);

    // Title bar — match book_viewer's build_page_at lines ~149-171.
    fmt.line_height_factor = 1.0;
    fmt.font_index         = book_viewer.TITLE_FONT;
    fmt.font_size          = book_viewer.TITLE_FONT_SIZE;
    fmt.font_style         = Fonts::FaceStyle::ITALIC;
    fmt.align              = CSS::Align::CENTER;

    if (show_title != 0) {
      const char * t = epub.get_title();
      char title_buf[55];
      if (strlen(t) > 50) {
        strncpy(title_buf, t, 50);
        title_buf[50] = 0;
        strcat(title_buf, "...");
      } else {
        strncpy(title_buf, t, 54);
        title_buf[54] = 0;
      }
      page.put_str_at(title_buf,
                      Pos(Page::HORIZONTAL_CENTER, title_baseline_offset),
                      fmt);
    }

    // Page-number / progress indicator at the bottom — match
    // book_viewer's ScreenBottom::show call at line 174 there.
    ScreenBottom::show(page_locs.get_page_nbr(page_id),
                       page_locs.get_page_count());

    // Emit the display list to the off-screen buffer. This is the
    // foreground path's `page.paint(FAST)` minus the screen.update —
    // see page.cpp::paint_to_active_target.
    page.paint_to_active_target(/*clear_screen=*/false, /*do_it=*/true);
  }
  interp.release_fmt(new_fmt);

  return true;
}

}  // namespace

#endif // EPUB_INKPLATE_BUILD && BOARD_TYPE_PAPER_S3
