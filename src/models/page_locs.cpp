// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#define __PAGE_LOCS__ 1
#include "models/page_locs.hpp"

#include "models/toc.hpp"
#include "models/config.hpp"
#include "models/fonts.hpp"
#include "models/epub.hpp"
#include "controllers/event_mgr.hpp"
#include "viewers/screen_bottom.hpp"

#include "viewers/book_viewer.hpp"
#include "viewers/msg_viewer.hpp"
#include "viewers/page.hpp"

#include <iostream>
#include <fstream>
#include <ios>
#include <memory>

// MgrReq used to discriminate two reply kinds on a shared
// mgr_queue. Now each kind has its own queue (asap_queue,
// stopped_queue), so the enum is mostly redundant — but keep
// it: it still shows up in log lines as a sanity tag, and
// keeping a shared MgrQueueData lets both queues use the same
// element size without two parallel structs.
enum class MgrReq : int8_t { ASAP_READY, STOPPED };

struct MgrQueueData {
  MgrReq req;
  int16_t itemref_index;
};

enum class StateReq  : int8_t { ABORT, STOP, START_DOCUMENT, GET_ASAP, ITEM_READY, ASAP_READY };

struct StateQueueData {
  StateReq req;
  int16_t itemref_index;
  int16_t itemref_count;
};

enum class RetrieveReq  : int8_t { ABORT, RETRIEVE_ITEM, GET_ASAP, SHOW_HEAP };

struct RetrieveQueueData {
  RetrieveReq req;
  int16_t itemref_index;
};

// Foreground-side bounded waits on the retriever's reply queues.
// Sized as broken-book detectors, not expected waits — see the
// commentary at the call sites and at mgr_reply_recv_timed_ms.
static constexpr uint32_t RETRIEVE_ASAP_TIMEOUT_MS  = 10000;  // 10 s
static constexpr uint32_t STOP_DOCUMENT_TIMEOUT_MS  =  5000;  //  5 s

#if EPUB_LINUX_BUILD
  #include <chrono>

  // Two reply queues, one per reply type. Splitting the
  // formerly-shared mgr_queue eliminates the latent race where
  // a STOPPED arriving on retrieve_asap's path (or vice versa)
  // would log "ERROR!!!" and mis-handle. Each consumer waits on
  // its own queue; cross-talk is structurally impossible.
  static mqd_t asap_queue;     // StateTask → retrieve_asap; carries MgrReq::ASAP_READY
  static mqd_t stopped_queue;  // StateTask → stop_document; carries MgrReq::STOPPED
  static mqd_t state_queue;
  static mqd_t retrieve_queue;

  static mq_attr asap_attr     = { 0, 5, sizeof(     MgrQueueData), 0 };
  static mq_attr stopped_attr  = { 0, 2, sizeof(     MgrQueueData), 0 };
  static mq_attr state_attr    = { 0, 5, sizeof(   StateQueueData), 0 };
  static mq_attr retrieve_attr = { 0, 5, sizeof(RetrieveQueueData), 0 };

  #define QUEUE_SEND(q, m, t)        mq_send(q, (const char *) &m, sizeof(m),       1)
  #define QUEUE_RECEIVE(q, m, t)  mq_receive(q,       (char *) &m, sizeof(m), nullptr)
#else
  #include <esp_pthread.h>
  #include <esp_heap_caps.h>
  #include <freertos/FreeRTOS.h>
  #include <freertos/task.h>

  static esp_pthread_cfg_t create_config(const char *name, int core_id, int stack, int prio)
  {
      auto cfg = esp_pthread_get_default_config();
      cfg.thread_name = name;
      cfg.pin_to_core = core_id;
      cfg.stack_size = stack;
      cfg.prio = prio;
      // Place these large pthread stacks in PSRAM. retrieverTask
      // (60 KB) and stateTask (10 KB) together would otherwise consume
      // ~70 KB of MALLOC_CAP_INTERNAL/MALLOC_CAP_DMA, leaving SD-SPI
      // bounce-buffer allocations (sdmmc_read_sectors) without enough
      // contiguous DMA memory at PageLocs::load time and aborting via
      // an unallocatable ios::failure throw. Neither thread passes
      // stack-resident buffers to a DMA peripheral; their work is XML
      // parsing, page formatting and PSRAM framebuffer writes — all
      // CPU-mediated — so the PSRAM access penalty is negligible
      // compared to the SD/iostream costs they're already bound by.
      // Requires CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y, present
      // in sdkconfig.paper_s3. MALLOC_CAP_8BIT is mandated by
      // esp_pthread_set_cfg.
      cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
      return cfg;
  }

  // See Linux block above for the rationale on splitting the
  // formerly-shared mgr_queue into per-reply-type queues.
  static QueueHandle_t asap_queue     = nullptr;  // → retrieve_asap (ASAP_READY)
  static QueueHandle_t stopped_queue  = nullptr;  // → stop_document (STOPPED)
  static QueueHandle_t state_queue    = nullptr;
  static QueueHandle_t retrieve_queue = nullptr;

  #define QUEUE_SEND(q, m, t)        xQueueSend(q, &m, t)
  #define QUEUE_RECEIVE(q, m, t)  xQueueReceive(q, &m, t)
#endif

// Bounded wait on a reply queue. Returns true on receive, false on
// timeout. Used by retrieve_asap and stop_document so the foreground
// can recover from a wedged retriever (e.g. FreeType infinite loop on
// a malformed embedded font, runaway CSS/layout) rather than blocking
// the panel forever on portMAX_DELAY. The retrieve_asap path was the
// concrete trigger: La Belgariade got the device stuck on "Retrieving
// Font(s)" indefinitely with no way out short of pulling the SD card.
static bool
mgr_reply_recv_timed_ms(
    #if EPUB_LINUX_BUILD
      mqd_t           q,
    #else
      QueueHandle_t   q,
    #endif
    MgrQueueData &  data,
    uint32_t        timeout_ms)
{
  #if EPUB_LINUX_BUILD
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec  +=  timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_sec  += 1;
      deadline.tv_nsec -= 1000000000L;
    }
    return mq_timedreceive(q, (char *) &data, sizeof(data), nullptr, &deadline) != -1;
  #else
    return xQueueReceive(q, &data, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
  #endif
}

// =====================================================================
// STOP / STOPPED handshake invariant
//
// PageLocs::stop_document() and several lifecycle paths rely on a
// specific guarantee: when STOPPED arrives back on stopped_queue, the
// RetrieverTask is GENUINELY IDLE — not just yielded mid-build.
//
// The handshake works like this:
//   1. caller -> state_queue: STOP
//   2. StateTask sees STOP, sets aborting=true, then either:
//        a) retriever already idle → stopped_queue: STOPPED immediately, or
//        b) sets stopping=true; STOPPED is emitted later when the
//           retriever completes its current build and emits ITEM_READY
//           (or ASAP_READY) back to StateTask, which then drains and
//           replies STOPPED.
//   3. RetrieverTask emits ITEM_READY/ASAP_READY only AFTER
//      build_page_locs returns. build_page_locs releases its
//      scoped_lock(book_viewer.get_mutex()) on the way out and
//      drops back to QUEUE_RECEIVE(retrieve_queue, ..., portMAX_DELAY).
//   4. So by the time STOPPED arrives at stopped_queue, the retriever
//      holds neither the book_viewer mutex nor any stack reference
//      into PageLocs::item_info — it's blocked in the queue receive.
//
// Earlier this all funneled through a single `mgr_queue` carrying
// both ASAP_READY and STOPPED, with each consumer logging "ERROR!!!"
// if it received the wrong reply type. The current single-threaded
// UI prevents the consumers from being in flight simultaneously, so
// the queue never actually mixed reply kinds — but a future async
// teardown could race. Splitting into asap_queue (→ retrieve_asap)
// and stopped_queue (→ stop_document) eliminates the latent ambiguity
// at the cost of one extra QueueHandle.
//
// This is what makes the clear_item_data(item_info) call inside
// stop_document safe: no concurrent reader, no in-flight pointer
// into the structure being freed. Any future change that emits
// STOPPED on a path that DOESN'T go through ITEM_READY/ASAP_READY
// (and therefore through build_page_locs's full unwind) MUST audit
// this guarantee or it can race against item_info teardown.
// =====================================================================
class StateTask
{
  private:
    static constexpr const char * TAG = "StateTask";

    bool retriever_iddle;

    int16_t   itemref_count;       // Number of items in the document
    int16_t   waiting_for_itemref; // Current item being processed by retrieval task
    int16_t   next_itemref_to_get; // Non prioritize item to get next
    int16_t   asap_itemref;        // Prioritize item to get next
    uint8_t * bitset;              // Set of all items processed so far
    uint16_t  bitset_size;         // bitset byte length (uint8_t silently
                                   // overflowed for books with >2040 spine
                                   // items, leading to a too-small alloc and
                                   // out-of-bounds bit access — heap corruption)
    bool      stopping;
    bool      forget_retrieval;    // Forget current item begin processed by retrieval task

    // Cooperative-abort flag observed by RetrieverTask's CPU-heavy paths
    // (build_page_locs / page_end). Set on STOP, cleared on START_DOCUMENT.
    // volatile is sufficient: both StateTask and RetrieverTask are pinned
    // to core 0 (see esp_pthread cfg below), so no cross-core ordering is
    // required. std::atomic would be over-engineering here.
    // Distinct from forget_retrieval, whose semantics ("discard partial
    // inserts") are set/cleared in multiple unrelated places.
    volatile bool aborting;

    StateQueueData       state_queue_data;
    RetrieveQueueData retrieve_queue_data;
    MgrQueueData           mgr_queue_data;

    /**
     * @brief Request next item to be retrieved
     *
     * This function is called to identify and send the
     * next request for retrieval of pages location. It also
     * identify when the whole process is completed, as all items from
     * the document have been done. It will then send this information
     * to the appliction through the Mgr queue.
     *
     * When this function is called, the retrieval task is waiting for
     * the next task to do.
     *
     * @param itemref The last itemref index that was processed
     */
    void request_next_item(int16_t itemref,
                          bool already_sent_to_mgr = false)
    {
      if (asap_itemref != -1) {
        if (itemref == asap_itemref) {
          asap_itemref = -1;
          if (!already_sent_to_mgr) {
            mgr_queue_data = {
              .req           = MgrReq::ASAP_READY,
              .itemref_index = itemref
            };
            QUEUE_SEND(asap_queue, mgr_queue_data, 0);
            LOG_D("Sent ASAP_READY to Mgr");
          }
        } else {
          waiting_for_itemref = asap_itemref;
          asap_itemref        = -1;
          retrieve_queue_data = {
            .req           = RetrieveReq::GET_ASAP,
            .itemref_index = waiting_for_itemref
          };
          QUEUE_SEND(retrieve_queue, retrieve_queue_data, 0);
          retriever_iddle = false;
          LOG_D("Sent GET_ASAP to Retriever");
          return;
        }
      }
      if (next_itemref_to_get != -1) {
        waiting_for_itemref = next_itemref_to_get;
        next_itemref_to_get = -1;
        retrieve_queue_data = {
          .req           = RetrieveReq::RETRIEVE_ITEM,
          .itemref_index = waiting_for_itemref
        };
        QUEUE_SEND(retrieve_queue, retrieve_queue_data, 0);
        retriever_iddle = false;
        LOG_D("Sent RETRIEVE_ITEM to Retriever");
      } else {
        int16_t newref;
        if (itemref != -1) {
          newref = (itemref + 1) % itemref_count;
        } else {
          itemref = 0;
          newref = 0;
        }
        while ((bitset[newref >> 3] & (1 << (newref & 7))) != 0) {
          newref = (newref + 1) % itemref_count;
          if (newref == itemref)
            break;
        }
        if (newref != itemref) {
          waiting_for_itemref = newref;
          retrieve_queue_data = {
            .req           = RetrieveReq::RETRIEVE_ITEM,
            .itemref_index = waiting_for_itemref
          };
          QUEUE_SEND(retrieve_queue, retrieve_queue_data, 0);
          retriever_iddle = false;
          LOG_D("Sent RETRIEVE_ITEM to Retriever");
        } else {
          page_locs.computation_completed();
          retriever_iddle = true;
        }
      }
    }

  public:
    StateTask() :
          retriever_iddle(   true),
            itemref_count(     -1),
      waiting_for_itemref(     -1),
      next_itemref_to_get(     -1),
             asap_itemref(     -1),
                   bitset(nullptr),
              bitset_size(      0),
                 stopping(  false),
         forget_retrieval(  false),
                 aborting(  false)  { }

    void operator()() {
      for(;;) {
        LOG_D("==> Waiting for request... <==");
        if (QUEUE_RECEIVE(state_queue, state_queue_data, portMAX_DELAY) == -1) {
          LOG_E("Receive error: %d: %s", errno, strerror(errno));
        }
        else switch (state_queue_data.req) {
          case StateReq::ABORT:
            return;

          case StateReq::STOP:
            LOG_D("-> STOP <-");
            // Set abort flag first so a long-running build_page_locs on
            // RetrieverTask sees it at its next cooperative_check() and
            // unwinds promptly, avoiding the stop_document() deadlock
            // window where StateTask blocks waiting for ITEM_READY.
            aborting         = true;
            itemref_count    = -1;
            forget_retrieval = true;
            if (bitset != nullptr) {
              delete [] bitset;
              bitset = nullptr;
            }
            if (retriever_iddle) {
              mgr_queue_data = {
                .req           = MgrReq::STOPPED,
                .itemref_index = 0
              };
              QUEUE_SEND(stopped_queue, mgr_queue_data, 0);
            }
            else {
              stopping = true;
            }
            break;

          case StateReq::START_DOCUMENT:
            LOG_D("-> START_DOCUMENT <-");
            // Fresh document: clear any stale abort signal from a prior STOP.
            aborting      = false;
            if (bitset) delete [] bitset;
            itemref_count = state_queue_data.itemref_count;
            // Guard against malformed EPUBs reporting <= 0 spine items.
            // Without this, bitset_size = (-1 + 7) >> 3 (or 0) leads to a
            // zero-byte allocation followed by indexed access at line ~283,
            // which is undefined behavior.
            if (itemref_count <= 0) {
              bitset           = nullptr;
              bitset_size      = 0;
              itemref_count    = -1;
              retriever_iddle  = true;
              forget_retrieval = true;
              // Reset stopping defensively: a prior STOP that left
              // stopping=true would otherwise cause the next
              // ITEM_READY/ASAP_READY (if any racy in-flight reply
              // arrives) to fire a spurious STOPPED into stopped_queue.
              stopping         = false;
              break;
            }
            bitset_size   = (uint16_t)((itemref_count + 7) >> 3);
            bitset        = new uint8_t[bitset_size];
            if (bitset) {
              memset(bitset, 0, bitset_size);
              if (waiting_for_itemref == -1) {
                retrieve_queue_data.req           = RetrieveReq::RETRIEVE_ITEM;
                retrieve_queue_data.itemref_index = waiting_for_itemref =
                                                    state_queue_data.itemref_index;
                forget_retrieval                  = false;
                QUEUE_SEND(retrieve_queue, retrieve_queue_data, 0);
                LOG_D("Sent RETRIEVE_ITEM to retriever");
              }
              else {
                forget_retrieval    = true;
                next_itemref_to_get = state_queue_data.itemref_index;
              }
              retriever_iddle = false;
            }
            else {
              itemref_count    = -1;
              retriever_iddle  = true;
              forget_retrieval = true;
            }
            break;

          case StateReq::GET_ASAP:
            LOG_D("-> GET_ASAP <-");
            // Mgr request a specific item. If document retrieval not started, 
            // return a negative value.
            // If already done, let it know it a.s.a.p. If currently being processed,
            // keep a mark when it will be back. If not, queue the request.
            if (itemref_count == -1) {
              mgr_queue_data = {
                .req           = MgrReq::ASAP_READY,
                .itemref_index = (int16_t) -(state_queue_data.itemref_index + 1)
              };
              QUEUE_SEND(asap_queue, mgr_queue_data, 0);
              LOG_D("Sent ASAP_READY to Mgr");
            }
            else {
              int16_t itemref = state_queue_data.itemref_index;
              if ((bitset[itemref >> 3] & ( 1 << (itemref & 7))) != 0) {
                mgr_queue_data = {
                  .req           = MgrReq::ASAP_READY,
                  .itemref_index = itemref
                };
                QUEUE_SEND(asap_queue, mgr_queue_data, 0);
                LOG_D("Sent ASAP_READY to Mgr");
              }
              else if (waiting_for_itemref != -1) {
                asap_itemref = itemref;
              }
              else {
                asap_itemref        = -1;
                waiting_for_itemref = itemref;
                retriever_iddle     = false;
                retrieve_queue_data = {
                  .req           = RetrieveReq::GET_ASAP,
                  .itemref_index = itemref
                };
                QUEUE_SEND(retrieve_queue, retrieve_queue_data, 0);       
                LOG_D("Sent GET_ASAP to Retriever"); 
              }
            }
            break;

          // This is sent by the retrieval task, indicating that an item has been
          // processed.
          case StateReq::ITEM_READY:
            LOG_D("-> ITEM_READY <-");
            waiting_for_itemref = -1;
            if (itemref_count != -1) {
              int16_t itemref = -1;
              if (forget_retrieval) {
                forget_retrieval = false;
              }
              else {
                itemref = state_queue_data.itemref_index;
                if (itemref < 0) {
                  itemref = -(itemref + 1);
                  LOG_E("Unable to retrieve pages location for item %d", itemref);
                }
                bitset[itemref >> 3] |= (1 << (itemref & 7));
              }
              if (stopping) {
                stopping = false;
                retriever_iddle  = true;
                mgr_queue_data = {
                  .req           = MgrReq::STOPPED,
                  .itemref_index = 0
                };
                QUEUE_SEND(stopped_queue, mgr_queue_data, 0);
              }
              else {
                request_next_item(itemref);
              }
            }
            else {
              if (stopping) {
                stopping = false;
                retriever_iddle  = true;
                mgr_queue_data = {
                  .req           = MgrReq::STOPPED,
                  .itemref_index = 0
                };
                QUEUE_SEND(stopped_queue, mgr_queue_data, 0);
              }
            }
            break;

          // This is sent by the retrieval task, indicating that an ASAP item has been
          // processed.
          case StateReq::ASAP_READY:
            LOG_D("-> ASAP_READY <-");
            waiting_for_itemref = -1;
            if (itemref_count != -1) {
              int16_t itemref = state_queue_data.itemref_index;
              // Correct the negative-encoded failure marker BEFORE sending
              // to asap_queue. The original code sent the raw (possibly
              // negative) itemref, so any future receiver that inspects
              // the index would see an out-of-range value. Match the
              // ITEM_READY handler's correct-then-use pattern.
              if (itemref < 0) {
                itemref = -(itemref + 1);
                LOG_E("Unable to retrieve pages location for item %d", itemref);
              }
              mgr_queue_data = {
                .req           = MgrReq::ASAP_READY,
                .itemref_index = itemref
              };
              QUEUE_SEND(asap_queue, mgr_queue_data, 0);
              LOG_D("Sent ASAP_READY to Mgr");
              bitset[itemref >> 3] |= (1 << (itemref & 7));
              if (stopping) {
                stopping = false;
                retriever_iddle  = true;
                mgr_queue_data = {
                  .req           = MgrReq::STOPPED,
                  .itemref_index = 0
                };
                QUEUE_SEND(stopped_queue, mgr_queue_data, 0);
              }
              else {
                request_next_item(itemref, true);
              }
            }
            else {
              if (stopping) {
                stopping = false;
                retriever_iddle  = true;
                mgr_queue_data = {
                  .req           = MgrReq::STOPPED,
                  .itemref_index = 0
                };
                QUEUE_SEND(stopped_queue, mgr_queue_data, 0);
              }
            }
            break;
        }
      }
    }

    inline bool   retriever_is_iddle() { return retriever_iddle;  }
    inline bool forgetting_retrieval() { return forget_retrieval; }
    inline bool          is_aborting() const { return aborting;   }

} state_task;

class RetrieverTask
{
  private:
    static constexpr const char * TAG = "RetrieverTask";

  public:
    void operator ()() const {
      RetrieveQueueData retrieve_queue_data;
      StateQueueData    state_queue_data;

      for (;;) {
        LOG_D("==> Waiting for request... <==");
        if (QUEUE_RECEIVE(retrieve_queue, retrieve_queue_data, portMAX_DELAY) == -1) {
          LOG_E("Receive error: %d: %s", errno, strerror(errno));
        }
        else {
          if (retrieve_queue_data.req == RetrieveReq::ABORT) return;
          if (retrieve_queue_data.req == RetrieveReq::SHOW_HEAP) {
            #if EPUB_INKPLATE_BUILD && (LOG_LOCAL_LEVEL == ESP_LOG_VERBOSE)
              ESP::show_heaps_info();
            #endif
            continue;
          }

          LOG_D("-> %s <-", (retrieve_queue_data.req == RetrieveReq::GET_ASAP) ? "GET_ASAP" : "RETRIEVE_ITEM");

          LOG_D("Retrieving itemref --> %d <--", retrieve_queue_data.itemref_index);

          // Per-item progress for the panel. The retriever runs a
          // long silent paginate-the-whole-book pass after the first
          // page is shown; before this, the user saw a stale "Retrieving
          // Font(s)" message indefinitely on a wedged item with no
          // signal of what step we were on. Shown only on GET_ASAP
          // (foreground-blocking item retrievals) so background
          // pagination doesn't redraw the panel underneath an
          // already-rendered book page.
          if (retrieve_queue_data.req == RetrieveReq::GET_ASAP) {
            const int16_t cur   = retrieve_queue_data.itemref_index + 1;
            const int16_t total = epub.get_item_count();
            msg_viewer.show(MsgViewer::MsgType::INFO,
                            false, false,
                            "Loading book",
                            "Paginating chapter %d of %d. Please wait.",
                            (int) cur, (int) total);
          }

          int16_t itemref_index;
          if (!page_locs.build_page_locs(retrieve_queue_data.itemref_index)) {
            // Unable to retrieve pages location for the requested index. Send back
            // a negative value to indicate the issue to the state task
            itemref_index = -(retrieve_queue_data.itemref_index + 1);
          }
          else {
            itemref_index = retrieve_queue_data.itemref_index;
          }

          //std::this_thread::sleep_for(std::chrono::seconds(5));
          state_queue_data = {
            .req = (retrieve_queue_data.req == RetrieveReq::GET_ASAP) ? 
                     StateReq::ASAP_READY : StateReq::ITEM_READY,
            .itemref_index = itemref_index,
            .itemref_count = 0
          };

          QUEUE_SEND(state_queue, state_queue_data, 0);
          LOG_D("Sent %s to State", (state_queue_data.req == StateReq::ASAP_READY) ? "ASAP_READY" : "ITEM_READY");
        }
      }
    }
} retriever_task;

void
PageLocs::setup()
{
  #if EPUB_LINUX_BUILD
    // Linux MQs persist in /dev/mqueue across program runs.
    // Unlink the new paths AND the legacy "/mgr" name a former
    // build of this binary may have left behind, so a dev-host
    // upgrade doesn't accumulate stale kernel state.
    mq_unlink("/mgr");
    mq_unlink("/asap");
    mq_unlink("/stopped");
    mq_unlink("/state");
    mq_unlink("/retrieve");

    asap_queue     = mq_open("/asap",     O_RDWR|O_CREAT, S_IRWXU, &asap_attr);
    if (asap_queue == -1) { LOG_E("Unable to open asap_queue: %d", errno); return; }

    stopped_queue  = mq_open("/stopped",  O_RDWR|O_CREAT, S_IRWXU, &stopped_attr);
    if (stopped_queue == -1) { LOG_E("Unable to open stopped_queue: %d", errno); return; }

    state_queue    = mq_open("/state",    O_RDWR|O_CREAT, S_IRWXU, &state_attr);
    if (state_queue == -1) { LOG_E("Unable to open state_queue: %d", errno); return; }

    retrieve_queue = mq_open("/retrieve", O_RDWR|O_CREAT, S_IRWXU, &retrieve_attr);
    if (retrieve_queue == -1) { LOG_E("Unable to open retrieve_queue: %d", errno); return; }

    retriever_thread = std::thread(retriever_task);
    state_thread     = std::thread(state_task);
  #else
    esp_pthread_init();

    // Reply queues from StateTask back to the foreground caller.
    // Depth 2 on stopped_queue is enough — stop_document is the
    // only consumer and at most one STOPPED is ever in flight.
    if (asap_queue     == nullptr) asap_queue     = xQueueCreate(5, sizeof(MgrQueueData));
    if (stopped_queue  == nullptr) stopped_queue  = xQueueCreate(2, sizeof(MgrQueueData));
    if (state_queue    == nullptr) state_queue    = xQueueCreate(5, sizeof(StateQueueData));
    if (retrieve_queue == nullptr) retrieve_queue = xQueueCreate(5, sizeof(RetrieveQueueData));

    auto cfg = create_config("retrieverTask", 0, 60 * 1024, configMAX_PRIORITIES - 2);
    cfg.inherit_cfg = true;
    esp_pthread_set_cfg(&cfg);
    retriever_thread = std::thread(retriever_task);
    
    cfg = create_config("stateTask", 0, 10 * 1024, configMAX_PRIORITIES - 2);
    cfg.inherit_cfg = true;
    esp_pthread_set_cfg(&cfg);
    state_thread = std::thread(state_task);
  #endif
} 

void
PageLocs::abort_threads()
{
  RetrieveQueueData retrieve_queue_data;
  retrieve_queue_data = {
    .req           = RetrieveReq::ABORT,
    .itemref_index = 0
  };
  LOG_D("abort_threads: Sending ABORT to Retriever");
  QUEUE_SEND(retrieve_queue, retrieve_queue_data, 0);

  retriever_thread.join();
  retriever_thread.~thread();
  
  StateQueueData state_queue_data;
  state_queue_data = {
    .req           = StateReq::ABORT,
    .itemref_index = 0,
    .itemref_count = 0
  };
  LOG_D("abort_threads: Sending ABORT to State");
  QUEUE_SEND(state_queue, state_queue_data, 0);

  state_thread.join();
  state_thread.~thread();
}

// Cooperative check called from inside RetrieverTask's CPU-heavy paths.
// Returns true if the caller should abort (a STOP was issued). Always
// yields to the scheduler so lower-priority UI tasks can run during long
// page-layout computation. Consolidates WDT-reset + yield + abort-check
// so the build loop never CPU-spins on core 0 at priority MAX-2 and
// starves the UI task.
static inline bool cooperative_check() {
  #if EPUB_INKPLATE_BUILD && !defined(BOARD_TYPE_PAPER_S3)
    esp_task_wdt_reset();
  #endif
  #if EPUB_LINUX_BUILD
    std::this_thread::yield();
  #else
    // 1 tick (~10 ms at default tick rate). Unlike taskYIELD()/
    // std::this_thread::yield(), vTaskDelay actually drops the running
    // task into the Blocked state, allowing any ready task at lower
    // priority on the same core to run.
    vTaskDelay(1);
  #endif
  return state_task.is_aborting();
}

class PageLocsInterp : public HTMLInterpreter
{
  public:
    PageLocsInterp(Page & the_page, DOM & the_dom, Page::ComputeMode the_comp_mode, const EPub::ItemInfo & the_item) :
      HTMLInterpreter(the_page, the_dom, the_comp_mode, the_item) {}
    ~PageLocsInterp() {}

    void doc_end(const Page::Format & fmt) { page_end(fmt); }

    // Hot-path abort check for inner build_pages_recurse loops. No
    // yield, no logging — just the flag fetch — because this is
    // called every ~64 characters and the cost compounds.
    bool should_abort_inner() const override { return state_task.is_aborting(); }

  protected:
    bool page_end(const Page::Format & fmt) {

      // if (page_locs.get_pages_map().size() == 38) {
      //   LOG_D("PAGE END!!");
      // }
      
      bool res = true;
      // if ((item_info.itemref_index == 0) || !page_out.is_empty()) {

        PageLocs::PageId   page_id   = PageLocs::PageId(page_locs.get_item_info().itemref_index, start_offset);
        PageLocs::PageInfo page_info = PageLocs::PageInfo(current_offset - start_offset, -1);
        
        if ((page_info.size > 0) || ((page_id.itemref_index == 0) && (page_id.offset == 0))) {
          if (page_info.size == 0) page_info.size = 1; // Patch for the case when it's the title page and no image is to be shown
          if ((page_locs.get_item_info().itemref_index > 0) && (page.is_empty())) {
            page_info.size = -page_info.size; // The page will not be counted nor displayed
          }
          res = page_locs.insert(page_id, page_info);
          #if DEBUGGING
            std::cout << page_id.offset << '|' 
                      << page_id.offset + page_info.size << ", " 
                      << page_info.page_number << ", " 
                      << page_info.size << std::endl;
          #endif
        }
        // Gives the chance to book_viewer to show a page if required.
        // Drop the mutex around the yield so the UI task can grab it,
        // then run cooperative_check() which (a) yields with real
        // scheduler descheduling (vTaskDelay(1) on FreeRTOS) and (b)
        // observes the aborting flag set by StateTask's STOP handler.
        book_viewer.get_mutex().unlock();
        const bool should_abort = cooperative_check();
        book_viewer.get_mutex().lock();
        if (should_abort) {
          // Return false up the build_pages_recurse chain. html_interpreter
          // already treats a false return from page_end as "abort the
          // recursion" (see all `if (!page_end(fmt)) return false;` sites
          // in src/viewers/html_interpreter.cpp), so build_page_locs's
          // `if (!interp->build_pages_recurse(...)) break;` path triggers
          // and falls through to the existing dom/css cleanup.
          return false;
        }

        // LOG_D("Page %d, offset: %d, size: %d", epub.get_page_count(), loc.offset, loc.size);
    
        #if DEBUGGING
          std::cout << page_locs.get_pages_map().size() << std::endl;
        #endif
        check_page_to_show(page_locs.get_pages_map().size()); // Debugging stuff
      //}

      start_offset = current_offset;

      page.start(fmt); // Start a new page
      // beginning_of_page = true;

      return res;
    }
};

bool
PageLocs::build_page_locs(int16_t itemref_index)
{
  // Fast-path: if a STOP arrived before we even started this item,
  // bail out before contending for the book_viewer mutex (which the
  // UI task may itself be waiting on).
  if (state_task.is_aborting()) return false;

  std::scoped_lock guard(book_viewer.get_mutex());

  Font * font = fonts.get(ScreenBottom::FONT);
  page_bottom = font->get_line_height(ScreenBottom::FONT_SIZE) + (font->get_line_height(ScreenBottom::FONT_SIZE) >> 1);
  
  //page_out.set_compute_mode(Page::ComputeMode::LOCATION);

  //show_images = current_format_params.show_images == 1;

  bool done = false;

  if (epub.get_item_at_index(itemref_index, item_info)) {

    int16_t idx;

    if ((idx = fonts.get_index("Fontbase", Fonts::FaceStyle::NORMAL)) == -1) {
      idx = 3;
    }
    
    int8_t font_size = current_format_params.font_size;

    int8_t show_title;
    config.get(Config::Ident::SHOW_TITLE, &show_title);

    int16_t page_top = 0;

    if (show_title != 0) {
      Font * title_font     = fonts.get(book_viewer.TITLE_FONT);
      page_top              = title_font->get_chars_height(book_viewer.TITLE_FONT_SIZE) + 10;
    }

    Page::Format fmt = {
      .line_height_factor = 0.95,
      .font_index         = idx,
      .font_size          = font_size,
      .indent             = 0,
      .margin_left        = 0,
      .margin_right       = 0,
      .margin_top         = 0,
      .margin_bottom      = 0,
      .screen_left        = 10,
      .screen_right       = 10,
      .screen_top         = page_top,
      .screen_bottom      = page_bottom,
      .width              = 0,
      .height             = 0,
      .vertical_align     = 0,
      .trim               = true,
      .pre                = false,
      .font_style         = Fonts::FaceStyle::NORMAL,
      .align              = CSS::Align::LEFT,
      .text_transform     = CSS::TextTransform::NONE,
      .display            = CSS::Display::INLINE
    };

    // RAII for the DOM and the PageLocsInterp so they get freed on
    // every return path — including stop_document mid-build, an
    // exception thrown from deep inside build_pages_recurse (pugixml
    // I/O, std::bad_alloc), or any future early-return added below.
    // The previous raw new/delete pattern leaked both allocations on
    // any path that didn't reach the explicit deletes at the end of
    // the function. Confirmed by inspection: with sequential book
    // loads + format-change aborts the leak compounded across loads,
    // matching the user-reported "device crashes after several books
    // are opened in sequence" symptom.
    auto dom    = std::make_unique<DOM>();
    auto interp = std::make_unique<PageLocsInterp>(page_out,
                                                   *dom,
                                                   Page::ComputeMode::LOCATION,
                                                   item_info);

    #if DEBUGGING_AID
      interp->set_pages_to_show_state(PAGE_FROM, PAGE_TO);
      interp->check_page_to_show(pages_map.size());
    #endif

    interp->set_limits(0,
                       9999999,
                       current_format_params.show_images == 1);

    while (!done) {

      // Cooperative check at the top of the build loop: yields to the UI
      // task and observes the abort flag. Subsumes the WDT-reset that
      // used to live just below page_out.start(fmt); doing it here means
      // we never enter build_pages_recurse (the deep-recursion CPU hog)
      // without first giving up the CPU and re-checking abort.
      if (cooperative_check()) break;

      // current_offset       = 0;
      // start_of_page_offset = 0;
      xml_node node;

      if ((node = item_info.xml_doc.child("html").child("body"))) {

        page_out.start(fmt);

        Page::Format * new_fmt = interp->duplicate_fmt(fmt);
        if (!interp->build_pages_recurse(node, *new_fmt, dom->body, 1)) {
          interp->release_fmt(new_fmt);
          LOG_D("html parsing issue or aborted by Mgr");
          break;
        }
        interp->release_fmt(new_fmt);

        if (page_out.some_data_waiting()) page_out.end_paragraph(fmt);
      }
      else {
        LOG_D("No <body>");
        break;
      }

      interp->doc_end(fmt);

      done = true;
    }

    // unique_ptr destructors fire here — dom and interp freed on every
    // path out of the if-block, including the loop break above.
  }

  //page_out.set_compute_mode(Page::ComputeMode::DISPLAY);

  if (item_info.css != nullptr) {
    delete item_info.css;
    item_info.css = nullptr;
  }

  return done;
}

volatile bool relax = false;

bool 
PageLocs::retrieve_asap(int16_t itemref_index) 
{
  StateQueueData state_queue_data;
  state_queue_data = {
    .req           = StateReq::GET_ASAP,
    .itemref_index = itemref_index,
    .itemref_count = 0
  };
  LOG_D("retrieve_asap: Sending GET_ASAP");
  QUEUE_SEND(state_queue, state_queue_data, 0);

  relax = true;
  MgrQueueData mgr_queue_data;
  LOG_D("==> Waiting for answer... <==");
  // asap_queue carries only ASAP_READY; STOPPED replies go to
  // stopped_queue. The mismatch case the original "ERROR!!!" log
  // guarded against can no longer occur structurally. Logging
  // the .req field anyway (always "ASAP_READY") keeps the trace
  // shape stable for any external log-watcher and earns the
  // discriminator field its keep.
  //
  // Bounded wait: if the retriever is genuinely wedged (a malformed
  // embedded font that loops FreeType, runaway CSS/layout for a
  // single item, etc.) the foreground would otherwise block the
  // panel forever on portMAX_DELAY — the symptom that wedged the
  // device on La Belgariade with no way out short of pulling the SD
  // card. RETRIEVE_ASAP_TIMEOUT_MS is a "broken book detector," not
  // an expected wait: a healthy book paginates the requested item
  // in <2s, so 10s leaves comfortable headroom on a slow SD without
  // letting a stuck retriever wedge the UI indefinitely.
  const bool got_reply = mgr_reply_recv_timed_ms(asap_queue, mgr_queue_data,
                                                RETRIEVE_ASAP_TIMEOUT_MS);
  relax = false;

  if (!got_reply) {
    LOG_E("retrieve_asap: %u ms timeout for itemref=%d. "
          "Retriever appears wedged; aborting load.",
          (unsigned) RETRIEVE_ASAP_TIMEOUT_MS, (int) itemref_index);
    // Deliberately NOT calling stop_document() here. Two reasons:
    //
    // 1. If the retriever is hung deep inside FreeType (the most
    //    likely failure mode) it's holding the Fonts mutex.
    //    EPub::close_file() — which BooksDirController::enter()
    //    invokes after stop_document() returns — calls fonts.clear()
    //    which tries to acquire that same mutex, deadlocking the
    //    foreground. Better to leave the retriever leaked than to
    //    cascade the wedge into the cleanup path.
    //
    // 2. open_book_file's failure handling (book_was_shown=false +
    //    NVS clear in BooksDirController::show_last_book) is enough
    //    to break the auto-resume loop. The user can power-cycle if
    //    the leaked retriever causes subsequent book opens to behave
    //    oddly; that's a vastly better outcome than the SD-card-pull
    //    they had to do before this fix.
    //
    // The state_task is left in "waiting_for_itemref != -1" /
    // "retriever_iddle = false" state. The next start_new_document
    // observes that and calls stop_document, which now hits its own
    // 5 s timeout and proceeds. Healthy books will then work; only
    // a re-attempt of the same wedge-trigger book will fail again.
    return false;
  }
  return true;
}

void
PageLocs::stop_document()
{
  StateQueueData state_queue_data;

  LOG_D("start_new_document: Sending STOP");
  state_queue_data = {
    .req           = StateReq::STOP,
    .itemref_index = 0,
    .itemref_count = 0
  };
  QUEUE_SEND(state_queue, state_queue_data, 0);

  MgrQueueData mgr_queue_data;
  LOG_D("==> Waiting for STOPPED... <==");
  // stopped_queue carries only STOPPED; ASAP_READY replies go to
  // asap_queue. No reply-type mismatch is possible — the receive
  // either delivers a STOPPED or blocks forever (which would
  // indicate a missing send, not a wrong-type send).
  //
  // Bounded wait for the same reason retrieve_asap is bounded: a
  // wedged retriever (FreeType infinite loop, etc.) would otherwise
  // never confirm STOPPED. STOP_DOCUMENT_TIMEOUT_MS is short — by
  // the time we reach stop_document the retriever has either
  // observed the aborting flag (cooperative_check yields every page
  // and a few hundred milliseconds is the worst-case build_pages_
  // recurse settling time on a healthy item) or it's hung. Any
  // longer just delays the foreground without changing the
  // outcome.
  if (!mgr_reply_recv_timed_ms(stopped_queue, mgr_queue_data,
                              STOP_DOCUMENT_TIMEOUT_MS)) {
    LOG_E("stop_document: %u ms timeout. Retriever did not confirm "
          "STOPPED — proceeding anyway. Subsequent operations may be "
          "unreliable until power cycle.",
          (unsigned) STOP_DOCUMENT_TIMEOUT_MS);
    return;
  }
  LOG_D("-> %s <-", (mgr_queue_data.req == MgrReq::STOPPED) ? "STOPPED" : "?");

  // Retriever is now confirmed idle; safe to free the per-item state
  // it was working on. Without this, item_info.xml_doc + css_cache +
  // css_list + data + css all hung around until the next get_item_at_
  // index, which may never come (user navigated to dir and stayed,
  // or sleep entry on a non-resume path). On format-change aborts
  // the css_list pointers also went stale because their backing
  // entries in EPub::css_cache were freed by close_file.
  //
  // Held under the same mutex as clear() to keep the invariant
  // "item_info is either valid-current-item OR fully-zeroed,
  // never partially-cleaned-while-someone-is-reading".
  std::scoped_lock guard(mutex);
  epub.clear_item_data(item_info);
}

void
PageLocs::start_new_document(int16_t count, int16_t itemref_index)
{
  // Phase logs (LOG_I, always-on). See open_file phase logs in
  // EPub::open_file for the rationale: a Belgariade-class wedge in
  // the open path was opaque on the panel and we want the serial
  // wire to localize the stuck step in one boot cycle.
  LOG_W("start_new_document: phase: stop_document_if_needed");
  if (!state_task.retriever_is_iddle()) stop_document();

  LOG_W("start_new_document: phase: load_cached_locs");
  const bool loaded = load(epub.get_current_filename());

  LOG_W("start_new_document: phase: check_for_format_changes (force=%d)",
        (int) !loaded);
  check_for_format_changes(count, itemref_index, !loaded);
  LOG_W("start_new_document: phase: DONE");
}

bool 
PageLocs::insert(PageId & id, PageInfo & info) 
{
  if (!state_task.forgetting_retrieval()) {
    while (true) {
      if (relax) {
        // The page_locs class is still in control of the mutex, but is waiting
        // for the completion of an GET_ASAP item. As such, it is safe to insert
        // a new page info in the list.
        LOG_D("Relaxed page info insert...");
        pages_map.insert(std::make_pair(id, info));
        items_set.insert(id.itemref_index);
        break;
      }
      else {
        if (mutex.try_lock_for(std::chrono::milliseconds(10))) {
          pages_map.insert(std::make_pair(id, info));
          items_set.insert(id.itemref_index);
          mutex.unlock();
          break;
        }
      }
    }
    return true;
  }
  return false;
}

PageLocs::PagesMap::iterator 
PageLocs::check_and_find(const PageId & page_id) 
{
  PagesMap::iterator it = pages_map.find(page_id);
  if (!completed && (it == pages_map.end())) {
    if (retrieve_asap(page_id.itemref_index)) it = pages_map.find(page_id);
  }
  return it;
}

const PageLocs::PageId * 
PageLocs::get_next_page_id(const PageId & page_id, int16_t count)
{
  std::scoped_lock guard(mutex);

  PagesMap::iterator it = check_and_find(page_id);
  if (it == pages_map.end()) {
    it = check_and_find(PageId(0,0));
  }
  else {
    PageId id = page_id;
    bool done = false;
    for (int16_t cptr = count; cptr > 0; cptr--) {
      PagesMap::iterator prev = it;
      do {
        id.offset += abs(it->second.size);
        it = pages_map.find(id);
        if (it == pages_map.end()) {
          // We have reached the end of the current item. Move to the next
          // item and try again
          id.itemref_index += 1; id.offset = 0;
          it = check_and_find(id);
          if (it == pages_map.end()) {
            // We have reached the end of the list. If stepping one page at a time, go
            // to the first page
            it = (count > 1) ? prev : check_and_find(PageId(0,0));
            done = true;
          }
        }
      } while ((it->second.size < 0) && !done);
      if (done) break;
    }
  }
  return (it == pages_map.end()) ? nullptr : &it->first;
}

const PageLocs::PageId * 
PageLocs::get_prev_page_id(const PageId & page_id, int count) 
{
  std::scoped_lock guard(mutex);

  PagesMap::iterator it = check_and_find(page_id);
  if (it == pages_map.end()) {
    it = check_and_find(PageId(0, 0));
  }
  else {
    PageId id = it->first;
    
    bool done = false;
    for (int16_t cptr = count; cptr > 0; cptr--) {
      do {
        if (id.offset == 0) {
          if (id.itemref_index == 0) {
            if (count == 1) id.itemref_index = item_count - 1;
            else done = true;
          }
          else id.itemref_index--;

          if (items_set.find(id.itemref_index) == items_set.end()) {
            retrieve_asap(id.itemref_index);
          }
        }
        
        if (!done) {
          if (it == pages_map.begin()) it = pages_map.end();
          it--;
          id = it->first;
        }
      } while ((it->second.size < 0) && !done);
      if (done) break;
    }
  }
  return (it == pages_map.end()) ? nullptr : &it->first;
}

const PageLocs::PageId * 
PageLocs::get_page_id(const PageId & page_id) 
{
  std::scoped_lock guard(mutex);

  PagesMap::iterator it     = check_and_find(PageId(page_id.itemref_index, 0));
  PagesMap::iterator result = pages_map.end();
  while ((it != pages_map.end()) && (it->first.itemref_index == page_id.itemref_index)) {
    if ((it->first.offset == page_id.offset) ||
        ((it->first.offset < page_id.offset) && ((it->first.offset + abs(it->second.size)) > page_id.offset))) { 
      result = it; 
      break; 
    }
    it++;
  }
  return (result == pages_map.end()) ? nullptr : &result->first ;
}

void
PageLocs::computation_completed()
{
  std::scoped_lock guard(mutex);

  if (!completed) {
    int16_t page_nbr = 0;
    for (auto & entry : pages_map) {
      if (entry.second.size >= 0) entry.second.page_number = page_nbr++;
    }

    page_count = page_nbr;

    save(epub.get_current_filename());
  
    //show();

    completed = true;
    toc.save();
    event_mgr.set_stay_on(false);
    // #if EPUB_INKPLATE_BUILD && (LOG_LOCAL_LEVEL == ESP_LOG_VERBOSE)
    //   ESP::show_heaps_info();
    //   RetrieveQueueData retrieve_queue_data = {
    //     .req = RetrieveReq::SHOW_HEAP,
    //     .itemref_index = 0
    //   };
    //   LOG_D("Sending SHOW_HEAP to Retriever");
    //   QUEUE_SEND(retrieve_queue, retrieve_queue_data, 0);
    // #endif
  }
}

#if DEBUGGING
  void
  PageLocs::show()
  {
    std::cout << "----- Page Locations -----" << std::endl;
    for (auto& entry : pages_map) {
      std::cout << " idx: " << entry.first.itemref_index
                << " off: " << entry.first.offset 
                << " siz: " << entry.second.size
                << " pg: "  << entry.second.page_number << std::endl;
    }
    std::cout << "----- End Page Locations -----" << std::endl;
  }
#endif

void
PageLocs::check_for_format_changes(int16_t count, int16_t itemref_index, bool force)
{
  if (force ||
      (memcmp(epub.get_book_format_params(), &current_format_params, sizeof(current_format_params)) != 0) ||
      !toc.load()) {

    LOG_W("check_for_format_changes: phase: recalc_required");

    if (!state_task.retriever_is_iddle()) {
      LOG_W("check_for_format_changes: phase: stop_document");
      stop_document();
    }

    LOG_W("check_for_format_changes: phase: clear_pages_map");
    clear();

    current_format_params = *epub.get_book_format_params();

    LOG_W("check_for_format_changes: phase: toc.load_from_epub");
    if (toc.load_from_epub() && !toc.there_is_some_ids()) {
      // The table of content doesn't need to be synch with the
      // page location computation. I.e. there is no relation with HTML Ids
      // that would require information from the page location computation
      // to find where the table of content pages are located.
      LOG_W("check_for_format_changes: phase: toc.save");
      toc.save();
    }

    item_count = count;
    StateQueueData state_queue_data;

    state_queue_data = {
      .req = StateReq::START_DOCUMENT,
      .itemref_index = itemref_index,
      .itemref_count = item_count
    };
    LOG_W("check_for_format_changes: phase: send_START_DOCUMENT");
    QUEUE_SEND(state_queue, state_queue_data, 0);

    event_mgr.set_stay_on(true);
    LOG_W("check_for_format_changes: phase: DONE");
  }
}

bool PageLocs::load(const std::string & epub_filename)
{
  std::string   filename = epub_filename.substr(0, epub_filename.find_last_of('.')) + ".locs";

  std::ifstream file(filename, std::ios::in | std::ios::binary);

  LOG_D("Loading pages location from file %s.", filename.c_str());

  int8_t  version;
  int16_t pg_count;

  if (!file.is_open()) {
    LOG_I("Unable to open pages location file. Calculing locations...");
    return false;
  }

  bool ok = false;
  while (true) {
    if (file.read(reinterpret_cast<char *>(&version), 1).fail()) break;
    if (version != LOCS_FILE_VERSION) break;

    if (file.read(reinterpret_cast<char *>(&current_format_params), sizeof(current_format_params)).fail()) break;
    if (file.read(reinterpret_cast<char *>(&pg_count),              sizeof(pg_count)             ).fail()) break;

    pages_map.clear();

    int16_t page_nbr = 0;

    for (int16_t i = 0; i < pg_count; i++) {
      PageId   page_id;
      PageInfo page_info;
      
      if (file.read(reinterpret_cast<char *>(&page_id.itemref_index), sizeof(page_id.itemref_index)).fail()) break;
      if (file.read(reinterpret_cast<char *>(&page_id.offset),        sizeof(page_id.offset       )).fail()) break;
      if (file.read(reinterpret_cast<char *>(&page_info.size),        sizeof(page_info.size       )).fail()) break;
      page_info.page_number = (page_info.size >= 0) ? page_nbr++ : -1;

      page_locs.insert(page_id, page_info);
    }

    page_count = page_nbr;
    ok = !file.fail();
    break;
  }

  file.close();

  LOG_D("Page locations load %s.", ok ? "Success" : "Error");

  completed = ok;
  
  return ok;
}

bool 
PageLocs::save(const std::string & epub_filename)
{
  std::string   filename = epub_filename.substr(0, epub_filename.find_last_of('.')) + ".locs";
  std::ofstream file(filename, std::ios::out | std::ios::binary);

  LOG_D("Saving pages location to file %s", filename.c_str());

  if (!file.is_open()) {
    LOG_E("Not able to open pages location file.");
    return false;
  }

  int16_t page_count = pages_map.size();

  while (true) {
    if (file.write(reinterpret_cast<const char *>(&LOCS_FILE_VERSION),     1                            ).fail()) break;
    if (file.write(reinterpret_cast<const char *>(&current_format_params), sizeof(current_format_params)).fail()) break;
    if (file.write(reinterpret_cast<const char *>(&page_count),            sizeof(page_count)           ).fail()) break;

    for (auto & page : pages_map) {
      if (file.write(reinterpret_cast<const char *>(&page.first.itemref_index), sizeof(page.first.itemref_index)).fail()) break;
      if (file.write(reinterpret_cast<const char *>(&page.first.offset),        sizeof(page.first.offset       )).fail()) break;
      if (file.write(reinterpret_cast<const char *>(&page.second.size),         sizeof(page.second.size        )).fail()) break;
    }

    break;
  }

  bool res = !file.fail();
  file.close();

  LOG_D("Page locations save %s.", res ? "Success" : "Error");

  return res;
}
