// Modifications in this file are part of N64ModernRuntime (GPL-3.0; see
// COPYING). The notices below describe the changes and their authorship,
// as required by GPL-3.0 §5(a). Original file copyright remains with the
// original authors.
//
// Modified 2026 by Matthew Stanley:
//   - Re-queue externals on full target OSMesgQueue (replaces silent drop
//     in dequeue_external_messages; see commit a14270c).
//   - Defensive boundary check + post-mortem dump in do_send to surface
//     corrupted OSMesgQueue pointers cleanly instead of host-side SEGV
//     (see commit bbd3f79).
//   - Queue-event ring buffer (always-on) + ultramodern_mesg_recent_copy
//     accessor for runner-side diagnostics (commit dd8137d).
//
// Copyright (c) 2026 Matthew Stanley
//
// ---------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <cstdio>

#include "blockingconcurrentqueue.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

// Unified post-mortem dump (src/main/post_mortem.cpp) — called from
// the do_send corruption guard so we capture full state when a
// corrupted OSMesgQueue ptr arrives.
extern "C" void psr_post_mortem_dump(const char* reason, void* fault_info);

#include <cstring>
extern "C" void psr_dump_rdram_snapshot(const char* name);
extern "C" void psr_dump_trace_snapshot(const char* name);
extern "C" void recomp_ultra_trace_record(const char* name, uint32_t pc, uint32_t a0,
                                         uint32_t a1, uint32_t a2, uint32_t a3);

// ── Queue-event ring buffer ───────────────────────────────────────────
//
// Always-on ring of recent osSendMesg/osRecvMesg/external-enqueue
// events. Used to diagnose softlocks where the game thread blocks on
// a queue and we need to know exactly what was sent/received around
// the freeze. Probes query the ring post-fact via the runner's
// debug_server (`mesg_recent` cmd) — never arm/capture/dump.
namespace mesg_log {
    enum Op : uint8_t {
        OP_SEND_GAME       = 1,  // osSendMesg from game thread
        OP_SEND_EXTERNAL   = 2,  // osSendMesg from non-game thread (queued externally)
        OP_RECV_ENTER      = 3,  // osRecvMesg called
        OP_RECV_BLOCK      = 4,  // osRecvMesg blocked (queue empty + OS_MESG_BLOCK)
        OP_RECV_RETURN_OK  = 5,  // osRecvMesg returned with a message
        OP_EXT_DEQ_OK      = 6,  // dequeue_external_messages drained one
        OP_EXT_DEQ_FULL    = 7,  // dequeue tried to send but target full → re-queued (reliable)
        OP_DO_SEND_BLOCK   = 8,  // do_send blocked sender on queue-full
        OP_EXT_DEQ_DROP    = 9,  // dequeue dropped a coalescible event (VI/PRENMI) on a full queue
        OP_DO_SEND_DROP    = 10, // do_send NOBLOCK send dropped on a full queue (caller saw send fail)
    };

    struct Event {
        uint64_t seq;
        uint64_t ms;
        uint32_t mq;          // OSMesgQueue pointer (gpr-style)
        uint32_t msg;         // message value (often pointer)
        uint32_t thread;      // current OSThread guest pointer, or 0
        uint16_t thread_id;   // current OSThread id, or 0 outside game threads
        uint16_t valid_before;
        uint16_t valid_after;
        uint8_t  op;
        uint8_t  block;       // 1 = OS_MESG_BLOCK, 0 = OS_MESG_NOBLOCK
        uint8_t  game_thread; // 1 = sender/receiver was game thread
        uint8_t  pad;
        uint16_t reserved;
    };

    // Bumped from 1024 to 65536 to span ~18 minutes of VI ticks at 60Hz —
    // 1024 was getting saturated by VI retraces (60/sec) within ~17 seconds,
    // burying any non-VI events from menu transitions before we could query.
    // 65536 events × 40 bytes ≈ 2.5 MB, acceptable for diagnostics.
    constexpr size_t RING_CAP = 65536;
    static Event ring[RING_CAP];
    static std::atomic<uint64_t> next_seq{0};
    static std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

    // Never-evict per-QUEUE table: keeps the last few events for every queue the
    // game ever touched, keyed by queue address (linear-probe hash). The 64K
    // event ring wraps under the steady-state retrace/audio flood, so a queue
    // that went silent early (e.g. a parked thread's reply queue) scrolls out
    // before a probe looks. This table answers "after thread X blocked on recv,
    // did anyone ever SEND to its queue?" regardless of how long ago that was.
    // Depth of the per-queue never-evict history. Bumped from 4 to 64 so a
    // low-traffic handshake can be reconstructed in full: a boot-init pipeline
    // (e.g. the t3->t5->t6->t10->t7 "DONE" chain) parks each thread on a
    // reply queue with only a handful of lifetime events; 4 slots showed the
    // tail but lost the ordering that led to the stall (which "go"/DONE send
    // was dropped on a transiently-full queue). 64 captures every such queue
    // in full (they have <64 lifetime events) and the recent tail of busy
    // queues. MUST be a power of two (mask below) and MUST stay in sync with
    // the consumer's QState in debug_server.cpp (the qstate_size guard there
    // silently skips the table on a size mismatch).
    constexpr uint32_t QEVENTS = 64;
    struct QState {
        uint32_t queue;            // 0 = empty slot
        uint32_t count;            // total events recorded for this queue
        Event     last[QEVENTS];   // ring of the last QEVENTS events on this queue
    };
    constexpr size_t QCAP = 1024;
    static QState qstates[QCAP];

    inline void qstate_push(uint32_t mq, const Event& e) {
        if (mq == 0) return;
        const size_t h = (mq >> 3) % QCAP;
        for (size_t probe = 0; probe < QCAP; probe++) {
            QState& q = qstates[(h + probe) % QCAP];
            if (q.queue == 0 || q.queue == mq) {
                q.queue = mq;
                q.last[q.count & (QEVENTS - 1)] = e;
                q.count++;
                return;
            }
        }
    }

    static inline uint64_t now_ms() {
        return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());
    }

    inline void current_thread(uint8_t* rdram, uint32_t& thread, uint16_t& thread_id) {
        thread = 0;
        thread_id = 0;
        if (rdram == nullptr) {
            return;
        }

        PTR(OSThread) self = ultramodern::this_thread();
        if (self == NULLPTR) {
            return;
        }

        OSThread* t = TO_PTR(OSThread, self);
        thread = static_cast<uint32_t>(self);
        thread_id = static_cast<uint16_t>(t->id);
    }

    // ── Temporary Stadium 2 tripwire ─────────────────────────────────
    // The reply queue at 0x800CDA10 is overwritten with fragment-67 code
    // some time before the boot stall. Message-queue events happen
    // thousands of times a second, so checking here pins the moment to a
    // narrow slice of the always-on trace. Fires once. Remove with the
    // other freeze diagnostics.
    static std::atomic<uint32_t> ps2_watch_reported{0};

    inline void ps2_watch_clobber(uint8_t* rdram) {
        if (rdram == nullptr) return;
        if (ps2_watch_reported.load(std::memory_order_relaxed)) return;
        // A live OSMesgQueue here has a small non-negative validCount and
        // msgCount; fragment code in those slots reads as huge/negative.
        uint32_t valid, count;
        std::memcpy(&valid, rdram + 0x000CDA10 + 8, 4);
        std::memcpy(&count, rdram + 0x000CDA10 + 16, 4);
        if ((int32_t)valid >= 0 && (int32_t)valid < 0x100 &&
            (int32_t)count >= 0 && (int32_t)count < 0x100) {
            return;
        }
        uint32_t expected = 0;
        if (!ps2_watch_reported.compare_exchange_strong(expected, 1)) return;
        uint32_t w0, w1;
        std::memcpy(&w0, rdram + 0x000CDA10, 4);
        std::memcpy(&w1, rdram + 0x000CDA10 + 4, 4);
        psr_dump_rdram_snapshot("rdram_clobber.bin");
        recomp_ultra_trace_record("~ps2_mq_clobber", 0,
                                  (uint32_t)ultramodern::this_thread(), w0, w1, valid);
    }

    inline void record(uint8_t* rdram, uint8_t op, uint32_t mq, uint32_t msg,
                       uint16_t vb, uint16_t va,
                       bool block, bool game_thread) {
        const uint64_t s = next_seq.fetch_add(1, std::memory_order_relaxed);
        uint32_t thread = 0;
        uint16_t thread_id = 0;
        current_thread(rdram, thread, thread_id);
        Event& e = ring[s % RING_CAP];
        e.seq = s;
        e.ms = now_ms();
        e.mq = mq;
        e.msg = msg;
        e.thread = thread;
        e.thread_id = thread_id;
        e.valid_before = vb;
        e.valid_after = va;
        e.op = op;
        e.block = block ? 1 : 0;
        e.game_thread = game_thread ? 1 : 0;
        e.pad = 0;
        e.reserved = 0;
        qstate_push(mq, e);
        ps2_watch_clobber(rdram);
    }
}

// Public dump: copies up to `cap` most-recent events (chronological)
// into `out`. Returns count written via `*n_written` and the global
// write_idx via `*next_seq_out`.
extern "C" void ultramodern_mesg_recent_copy(
    void* out_void, size_t cap, size_t* n_written, uint64_t* next_seq_out)
{
    using namespace mesg_log;
    const uint64_t s = next_seq.load(std::memory_order_relaxed);
    if (next_seq_out) *next_seq_out = s;
    if (cap == 0 || out_void == nullptr) {
        if (n_written) *n_written = 0;
        return;
    }
    const size_t available = std::min<size_t>(s, RING_CAP);
    const size_t want = std::min(cap, available);
    Event* out = static_cast<Event*>(out_void);
    // Copy oldest first: starting index = (s - want) % RING_CAP.
    const size_t start = (s - want) % RING_CAP;
    for (size_t i = 0; i < want; i++) {
        out[i] = ring[(start + i) % RING_CAP];
    }
    if (n_written) *n_written = want;
}

extern "C" size_t ultramodern_mesg_event_size(void) {
    return sizeof(mesg_log::Event);
}

extern "C" size_t ultramodern_mesg_qstate_size(void) {
    return sizeof(mesg_log::QState);
}

// Copy the never-evict per-queue table (every queue ever touched, each with its
// last QEVENTS events). Lets a probe see a parked thread's reply-queue history
// even after the event ring has wrapped.
extern "C" void ultramodern_mesg_qstates_copy(
    void* out_void, size_t cap, size_t* n_written)
{
    using namespace mesg_log;
    size_t w = 0;
    if (out_void != nullptr) {
        QState* out = static_cast<QState*>(out_void);
        for (size_t i = 0; i < QCAP && w < cap; i++) {
            if (qstates[i].queue != 0) {
                out[w++] = qstates[i];
            }
        }
    }
    if (n_written) *n_written = w;
}

struct QueuedMessage {
    PTR(OSMesgQueue) mq;
    OSMesg mesg;
    bool jam;
    // false = reliable (re-queue until delivered); true = coalescible (may be
    // dropped on a full target). See post_rcp_event(). Generic external posts
    // default to reliable to preserve prior behavior.
    bool coalescible = false;
};

static moodycamel::BlockingConcurrentQueue<QueuedMessage> external_messages {};
static std::atomic<uint32_t> g_external_pending{0};

bool do_send(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, bool jam, bool block);

static void enqueue_external_tagged(PTR(OSMesgQueue) mq, OSMesg msg, bool jam, bool coalescible) {
    external_messages.enqueue({mq, msg, jam, coalescible});
    g_external_pending.fetch_add(1, std::memory_order_relaxed);
    mesg_log::record(nullptr, mesg_log::OP_SEND_EXTERNAL,
                     uint32_t(mq), uint32_t(uintptr_t(msg)),
                     0, 0, false, false);
}

void enqueue_external_message(PTR(OSMesgQueue) mq, OSMesg msg, bool jam) {
    // Generic host posts are reliable (re-queued until delivered).
    enqueue_external_tagged(mq, msg, jam, /*coalescible=*/false);
}

// RCP hardware-event bridge — see ultramodern.hpp. Runtime (host) producers call
// this with the event's reliability class. Mirrors osSendMesg's host path
// (always external; reliable events never lost, coalescible events droppable on
// a full guest queue). If ever called from a game thread, fall back to a direct
// send to keep ordering correct.
s32 ultramodern::post_rcp_event(RDRAM_ARG PTR(OSMesgQueue) mq, OSMesg msg, bool coalescible) {
    if (ultramodern::is_game_thread()) {
        do_send(PASS_RDRAM mq, msg, false, false);
        return 0;
    }
    enqueue_external_tagged(mq, msg, /*jam=*/false, coalescible);
    return 0;
}

// Counter for how many external messages have been re-queued after a
// transient target-queue-full failure. Surfaced via the runner's
// status command so a sustained nonzero value indicates a target
// queue is being overrun (receiver thread starved).
static std::atomic<uint64_t> g_external_requeues{0};

static bool deliver_waited_external(RDRAM_ARG const QueuedMessage& to_send) {
    OSMesgQueue* mq_pre = TO_PTR(OSMesgQueue, to_send.mq);
    const uint16_t vb = uint16_t(mq_pre ? mq_pre->validCount : 0);
    const bool ok = do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false);
    if (ok) {
        OSMesgQueue* mq_post = TO_PTR(OSMesgQueue, to_send.mq);
        const uint16_t va = uint16_t(mq_post ? mq_post->validCount : 0);
        mesg_log::record(rdram, mesg_log::OP_EXT_DEQ_OK,
                         uint32_t(to_send.mq),
                         uint32_t(uintptr_t(to_send.mesg)),
                         vb, va, false, true);
        return true;
    }

    if (to_send.coalescible) {
        mesg_log::record(rdram, mesg_log::OP_EXT_DEQ_DROP,
                         uint32_t(to_send.mq),
                         uint32_t(uintptr_t(to_send.mesg)),
                         vb, vb, false, true);
        return false;
    }

    mesg_log::record(rdram, mesg_log::OP_EXT_DEQ_FULL,
                     uint32_t(to_send.mq),
                     uint32_t(uintptr_t(to_send.mesg)),
                     vb, vb, false, true);
    external_messages.enqueue(to_send);
    g_external_pending.fetch_add(1, std::memory_order_relaxed);
    g_external_requeues.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void dequeue_external_messages(RDRAM_ARG1) {
    QueuedMessage to_send;
    while (external_messages.try_dequeue(to_send)) {
        g_external_pending.fetch_sub(1, std::memory_order_relaxed);
        OSMesgQueue* mq_pre = TO_PTR(OSMesgQueue, to_send.mq);
        const uint16_t vb = uint16_t(mq_pre ? mq_pre->validCount : 0);
        // Try non-blocking send first.
        bool ok = do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false);
        if (!ok) {
            // Target OSMesgQueue is full. Behavior now depends on the event's
            // reliability class (see post_rcp_event / ultramodern.hpp):
            //
            //  - COALESCIBLE (VI retrace / PRENMI): DROP it and keep draining.
            //    A missed retrace is just a missed frame tick (hardware-accurate
            //    on a full queue). Critically, this is what BREAKS the boot
            //    lost-wakeup: a game that points OS_EVENT_SP/DP/VI at one shared
            //    OSMesgQueue (PMS-J's osScheduler interruptQ) gets a 60Hz VI
            //    flood; re-queuing those VIs to the tail perpetually starved the
            //    SP/DP gfx-task-completion events behind them, so osScheduler
            //    never marked the task done and the boot pipeline parked. VI must
            //    not accumulate in the external queue.
            //
            //  - RELIABLE (SP/DP/SI/AI/PI completions + generic host posts):
            //    NEVER drop. These release blocked tasks/threads; losing one
            //    lost-wakeups the guest. Re-queue at the tail and bail this drain
            //    pass (we must NOT spin re-enqueuing with no chance for the
            //    receiver to drain). The caller (the game thread inside
            //    osSendMesg/osRecvMesg) returns and runs, the receiver dequeues
            //    and frees space, and the next dequeue_external_messages retries.
            //    With VI now dropped instead of re-queued, the reliable event is
            //    no longer starved.
            //
            // Historical context: Pokemon Stadium hit the reliable case on its
            // gfx scheduler queue 0x800A6CD0 — audio SP DONE + VI ticks (~110
            // events/sec) briefly filled a cap=16 queue and the gfx RDP DONE for
            // the third boot-logo dlist was dropped, softlocking Game_Thread. The
            // original fix re-queued EVERYTHING (incl. VI); this refines it so VI
            // coalesces and only true completions are guaranteed.
            //
            // NB: doesn't apply to OS_MESG_BLOCK semantics — that path is direct
            // (skips this external queue).
            if (to_send.coalescible) {
                mesg_log::record(rdram, mesg_log::OP_EXT_DEQ_DROP,
                                 uint32_t(to_send.mq),
                                 uint32_t(uintptr_t(to_send.mesg)),
                                 vb, vb, false, true);
                continue;
            }
            mesg_log::record(rdram, mesg_log::OP_EXT_DEQ_FULL,
                             uint32_t(to_send.mq),
                             uint32_t(uintptr_t(to_send.mesg)),
                             vb, vb, false, true);
            external_messages.enqueue(to_send);
            g_external_pending.fetch_add(1, std::memory_order_relaxed);
            g_external_requeues.fetch_add(1, std::memory_order_relaxed);
            break;
        } else {
            OSMesgQueue* mq_post = TO_PTR(OSMesgQueue, to_send.mq);
            const uint16_t va = uint16_t(mq_post ? mq_post->validCount : 0);
            mesg_log::record(rdram, mesg_log::OP_EXT_DEQ_OK,
                             uint32_t(to_send.mq),
                             uint32_t(uintptr_t(to_send.mesg)),
                             vb, va, false, true);
        }
    }
}

// Public accessor surfaced via the runner's debug_server status cmd.
extern "C" uint64_t ultramodern_external_requeues(void) {
    return g_external_requeues.load(std::memory_order_relaxed);
}

// Depth of the host->guest external message queue. Pairs with the requeue
// counter when diagnosing a lost wakeup: a nonzero pending count while the
// target guest thread is parked means delivery is stuck, not production.
extern "C" uint32_t ultramodern_external_pending(void) {
    return g_external_pending.load(std::memory_order_relaxed);
}

#ifdef N64_COSIM
extern "C" uint32_t ultramodern_cosim_checkpoint_external_pending(void) {
    if (g_external_pending.load(std::memory_order_relaxed) != 0) {
        return 1;
    }
    return ultramodern_cosim_rcp_event_due();
}
#endif

void ultramodern::wait_for_external_message(RDRAM_ARG1) {
    QueuedMessage to_send;
#ifdef N64_COSIM
    while (true) {
        if (external_messages.try_dequeue(to_send)) {
            g_external_pending.fetch_sub(1, std::memory_order_relaxed);
            deliver_waited_external(PASS_RDRAM to_send);
            return;
        }
        if (ultramodern_cosim_advance_to_next_rcp_event(rdram) != 0) {
            return;
        }
        if (external_messages.wait_dequeue_timed(to_send, std::chrono::milliseconds{1})) {
            g_external_pending.fetch_sub(1, std::memory_order_relaxed);
            deliver_waited_external(PASS_RDRAM to_send);
            return;
        }
    }
#else
    if (external_messages.try_dequeue(to_send)) {
        g_external_pending.fetch_sub(1, std::memory_order_relaxed);
        deliver_waited_external(PASS_RDRAM to_send);
        return;
    }
    external_messages.wait_dequeue(to_send);
    g_external_pending.fetch_sub(1, std::memory_order_relaxed);
    deliver_waited_external(PASS_RDRAM to_send);
#endif
}

void ultramodern::wait_for_external_message_timed(RDRAM_ARG1, u32 millis) {
    QueuedMessage to_send;
#ifdef N64_COSIM
    if (ultramodern_cosim_deliver_due_rcp_events(rdram) != 0) {
        return;
    }
#endif
    if (external_messages.wait_dequeue_timed(to_send, std::chrono::milliseconds{millis})) {
        g_external_pending.fetch_sub(1, std::memory_order_relaxed);
        deliver_waited_external(PASS_RDRAM to_send);
    }
}

bool ultramodern::external_message_pending() {
    if (g_external_pending.load(std::memory_order_relaxed) != 0) {
        return true;
    }
#ifdef N64_COSIM
    return ultramodern_cosim_rcp_event_pending() != 0;
#else
    return false;
#endif
}

void ultramodern::send_external_message_after(
    RDRAM_ARG PTR(OSMesgQueue) mq, OSMesg msg, u32 delay_us)
{
    (void)rdram;
    if (delay_us == 0) {
        enqueue_external_message(mq, msg, false);
        return;
    }
    std::thread{[mq, msg, delay_us]() {
        if (delay_us >= 1000) {
            std::this_thread::sleep_for(std::chrono::microseconds{delay_us});
        } else {
            std::this_thread::yield();
        }
        enqueue_external_message(mq, msg, false);
    }}.detach();
}

extern "C" void osCreateMesgQueue(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg, s32 count) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    mq->blocked_on_recv = NULLPTR;
    mq->blocked_on_send = NULLPTR;
    mq->msgCount = count;
    mq->msg = msg;
    mq->validCount = 0;
    mq->first = 0;
}

s32 MQ_GET_COUNT(OSMesgQueue *mq) {
    return mq->validCount;
}

s32 MQ_IS_EMPTY(OSMesgQueue *mq) {
    return mq->validCount == 0;
}

s32 MQ_IS_FULL(OSMesgQueue* mq) {
    return MQ_GET_COUNT(mq) >= mq->msgCount;
}

// ── macOS boot diagnostics: first-touch queue tracing (PSR_QTRACE=1) ────────
// One line the first time any queue is SENT to, and one the first time a
// thread BLOCKS receiving on it. That is exactly enough to separate a queue
// nobody ever sends to (a real deadlock) from one that is merely idle at the
// instant a backtrace was taken. Off unless PSR_QTRACE=1; no cost when off.
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_set>
static bool psr_qtrace_on() {
    static const bool on = [] {
        const char* e = std::getenv("PSR_QTRACE");
        return e != nullptr && e[0] == '1';
    }();
    return on;
}
static void psr_qtrace(const char* what, uint32_t q) {
    if (!psr_qtrace_on()) return;
    static std::mutex m;
    static std::unordered_set<uint64_t> seen;
    const uint64_t key = (((uint64_t)(uintptr_t)what) << 32) ^ (uint64_t)q;
    std::lock_guard<std::mutex> lk(m);
    if (!seen.insert(key).second) return;
    fprintf(stderr, "[qtrace] first %-10s queue 0x%08X\n", what, q);
    fflush(stderr);
}

bool do_send(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, bool jam, bool block) {
    psr_qtrace("send->", (uint32_t)mq_);
    // Defensive boundary check: do_send writes through mq->msg[last]
    // which produces a host-pointer SEGV if mq is corrupted (we've
    // observed thread20_rsp / OSScTask scenarios where the task
    // struct's msgQ field at +0x28 reads back garbage). Without this
    // guard the trace just shows "do_send wrote to host 0x65b075d8"
    // which is hard to backtrack. Guard:
    //   1. mq_ vaddr must be in kseg0 RDRAM range.
    //   2. mq->msg buffer pointer must be in kseg0 RDRAM range.
    //   3. mq->msgCount must be > 0 (else mod-by-zero on `last`).
    //   4. mq->first / validCount must be sane (< msgCount).
    // On failure: log to stderr, dump post-mortem, return false. The
    // caller treats this as "send failed" (same as a full queue) which
    // is recoverable; better than SEGV-and-die. The accompanying
    // post-mortem records who held the bad mq_ at the time so we can
    // chase the upstream corruption next iteration.
    auto fail = [&](const char* why, uint32_t a, uint32_t b, uint32_t c) -> bool {
        psr_dump_trace_snapshot("trace_clobber.log");
        psr_dump_rdram_snapshot("rdram_clobber.bin");
        fprintf(stderr,
            "[do_send] BAD mq_=0x%08X reason=%s a=0x%08X b=0x%08X c=0x%08X "
            "msg=0x%08X jam=%d block=%d — skipping send\n",
            uint32_t(mq_), why, a, b, c,
            uint32_t(uintptr_t(msg)), int(jam), int(block));
        fflush(stderr);
        // Trigger unified post-mortem so we get host-stack + rings +
        // hardware state dumped to last_run_report.json. Use a one-shot
        // guard so a stuck audio thread can't spam dumps.
        static std::atomic<int> dumped{0};
        int prior = dumped.fetch_add(1);
        if (prior == 0) {
            psr_post_mortem_dump("do_send-bad-mq", nullptr);
        }
        return false;
    };
    if (uint32_t(mq_) < 0x80000000u || uint32_t(mq_) >= 0x80800000u) {
        return fail("mq_-not-kseg0", 0, 0, 0);
    }
    OSMesgQueue* mq = TO_PTR(OSMesgQueue, mq_);
    if (uint32_t(mq->msg) < 0x80000000u || uint32_t(mq->msg) >= 0x80800000u) {
        return fail("mq.msg-not-kseg0",
                    uint32_t(mq->msg), uint32_t(mq->msgCount), 0);
    }
    if (mq->msgCount <= 0 || mq->msgCount > 0x10000) {
        return fail("mq.msgCount-insane",
                    uint32_t(mq->msg), uint32_t(mq->msgCount), 0);
    }
    if (mq->first < 0 || mq->first >= mq->msgCount
        || mq->validCount < 0 || mq->validCount > mq->msgCount) {
        return fail("mq.first/validCount-insane",
                    uint32_t(mq->first), uint32_t(mq->validCount),
                    uint32_t(mq->msgCount));
    }
    if (!block) {
        // Non-blocking send: report failure rather than wait when the queue is at capacity.
        if (MQ_IS_FULL(mq)) {
            // Make the drop LOUD: a NOBLOCK send to a full queue is silently
            // lost (the caller just sees "send failed"). Under the cooperative
            // scheduler a queue can be transiently full where preemptive HW
            // wouldn't be, so this is the prime suspect for boot-handshake
            // lost-wakeups (e.g. GB Tower #17). Record it in the always-on ring
            // so a probe can see WHICH wakeup was dropped, on which queue, by
            // which thread — instead of an invisible return false. (Observability
            // only; send semantics unchanged until the cause is confirmed.)
            mesg_log::record(rdram, mesg_log::OP_DO_SEND_DROP,
                             uint32_t(mq_), uint32_t(uintptr_t(msg)),
                             uint16_t(mq->validCount), uint16_t(mq->validCount),
                             false, true);
            return false;
        }
    }
    else {
        // Blocking send: park this thread and let others run until space frees up.
        while (MQ_IS_FULL(mq)) {
            mesg_log::record(rdram, mesg_log::OP_DO_SEND_BLOCK,
                             uint32_t(mq_), uint32_t(uintptr_t(msg)),
                             uint16_t(mq->validCount), uint16_t(mq->validCount),
                             true, true);
            debug_printf("[Message Queue] Thread %d is blocked on send\n", TO_PTR(OSThread, ultramodern::this_thread())->id);
#ifdef N64_COSIM
            ultramodern_cosim_scheduler_handoff_begin();
#endif
            ultramodern::thread_queue_insert(PASS_RDRAM GET_MEMBER(OSMesgQueue, mq_, blocked_on_send), ultramodern::this_thread());
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
        }
    }
    
    if (jam) {
        // A jam goes to the front of the circular buffer, becoming the next message read.
        mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
        TO_PTR(OSMesg, mq->msg)[mq->first] = msg;
        mq->validCount++;
    }
    else {
        // A normal send appends to the back of the circular buffer.
        s32 last = (mq->first + mq->validCount) % mq->msgCount;
        TO_PTR(OSMesg, mq->msg)[last] = msg;
        mq->validCount++;
    }

    // A successful send may unblock a receiver: wake the first one waiting on this queue.
    PTR(PTR(OSThread)) blocked_queue = GET_MEMBER(OSMesgQueue, mq_, blocked_on_recv);
    if (!ultramodern::thread_queue_empty(PASS_RDRAM blocked_queue)) {
        ultramodern::schedule_running_thread(PASS_RDRAM ultramodern::thread_queue_pop(PASS_RDRAM blocked_queue));
    }
    
    return true;
}

bool do_recv(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg_, bool block) {
    OSMesgQueue* mq = TO_PTR(OSMesgQueue, mq_);
    if (!block) {
        // Non-blocking receive: report failure rather than wait when there is nothing to read.
        if (MQ_IS_EMPTY(mq)) {
            return false;
        }
    } else {
        // Blocking receive: park this thread and let others run until a message arrives.
        while (MQ_IS_EMPTY(mq)) {
            mesg_log::record(rdram, mesg_log::OP_RECV_BLOCK,
                             uint32_t(mq_), 0,
                             uint16_t(mq->validCount), uint16_t(mq->validCount),
                             true, true);
            debug_printf("[Message Queue] Thread %d is blocked on receive\n", TO_PTR(OSThread, ultramodern::this_thread())->id);
#ifdef N64_COSIM
            ultramodern_cosim_scheduler_handoff_begin();
#endif
            psr_qtrace("block-recv", (uint32_t)mq_);
            ultramodern::thread_queue_insert(PASS_RDRAM GET_MEMBER(OSMesgQueue, mq_, blocked_on_recv), ultramodern::this_thread());
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
        }
    }

    if (msg_ != NULLPTR) {
        *TO_PTR(OSMesg, msg_) = TO_PTR(OSMesg, mq->msg)[mq->first];
    }
    
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;

    // Freeing a slot may unblock a sender: wake the first one waiting on this queue.
    PTR(PTR(OSThread)) blocked_queue = GET_MEMBER(OSMesgQueue, mq_, blocked_on_send);
    if (!ultramodern::thread_queue_empty(PASS_RDRAM blocked_queue)) {
        ultramodern::schedule_running_thread(PASS_RDRAM ultramodern::thread_queue_pop(PASS_RDRAM blocked_queue));
    }

    return true;
}

extern "C" s32 osSendMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, s32 flags) {
    // Do not form a host pointer until the guest queue address is validated.
    // do_send performs the deeper queue-state validation below.
    if (uint32_t(mq_) < 0x80000000u || uint32_t(mq_) >= 0x80800000u) {
        fprintf(stderr, "[osSendMesg] invalid guest queue 0x%08X; dropping message\\n", uint32_t(mq_));
        return -1;
    }

    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    bool jam = false;

    // Posts from non-game threads route through the external queue so only the
    // game thread ever touches the guest message buffer directly.
    if (!ultramodern::is_game_thread()) {
        enqueue_external_message(mq_, msg, jam);
        return 0;
    }

    // Flush anything other threads posted before performing this send.
    dequeue_external_messages(PASS_RDRAM1);

    const uint16_t vb = uint16_t(mq ? mq->validCount : 0);
    // Perform the send itself.
    bool sent = do_send(PASS_RDRAM mq_, msg, jam, flags == OS_MESG_BLOCK);
    const uint16_t va = uint16_t(mq ? mq->validCount : 0);
    mesg_log::record(rdram, mesg_log::OP_SEND_GAME,
                     uint32_t(mq_), uint32_t(uintptr_t(msg)),
                     vb, va, flags == OS_MESG_BLOCK, true);

    // A higher-priority thread may now be runnable; reschedule if so.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return sent ? 0 : -1;
}

extern "C" s32 osJamMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, s32 flags) {
    // Do not form a host pointer until the guest queue address is validated.
    // do_send performs the deeper queue-state validation below.
    if (uint32_t(mq_) < 0x80000000u || uint32_t(mq_) >= 0x80800000u) {
        fprintf(stderr, "[osJamMesg] invalid guest queue 0x%08X; dropping message\\n", uint32_t(mq_));
        return -1;
    }

    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    bool jam = true;

    // Posts from non-game threads route through the external queue so only the
    // game thread ever touches the guest message buffer directly.
    if (!ultramodern::is_game_thread()) {
        enqueue_external_message(mq_, msg, jam);
        return 0;
    }

    // Flush anything other threads posted before performing this jam.
    dequeue_external_messages(PASS_RDRAM1);

    // Perform the jam itself.
    bool sent = do_send(PASS_RDRAM mq_, msg, jam, flags == OS_MESG_BLOCK);

    // A higher-priority thread may now be runnable; reschedule if so.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return sent ? 0 : -1;
}

extern "C" s32 osRecvMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg_, s32 flags) {
    // Do not form a host pointer until the guest queue address is validated.
    // do_recv performs the deeper queue-state validation below.
    if (uint32_t(mq_) < 0x80000000u || uint32_t(mq_) >= 0x80800000u) {
        fprintf(stderr, "[osRecvMesg] invalid guest queue 0x%08X; dropping receive\\n", uint32_t(mq_));
        return -1;
    }

    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);

    assert(ultramodern::is_game_thread() && "RecvMesg not allowed outside of game threads.");

    const uint16_t vb_enter = uint16_t(mq ? mq->validCount : 0);
    mesg_log::record(rdram, mesg_log::OP_RECV_ENTER,
                     uint32_t(mq_), 0, vb_enter, vb_enter,
                     flags == OS_MESG_BLOCK, true);

    // Flush anything other threads posted before attempting the receive.
    dequeue_external_messages(PASS_RDRAM1);

    // Perform the receive itself.
    bool received = do_recv(PASS_RDRAM mq_, msg_, flags == OS_MESG_BLOCK);
    const uint16_t va = uint16_t(mq ? mq->validCount : 0);
    if (received) {
        uint32_t got = 0;
        if (msg_ != NULLPTR) {
            got = uint32_t(uintptr_t(*TO_PTR(OSMesg, msg_)));
        }
        mesg_log::record(rdram, mesg_log::OP_RECV_RETURN_OK,
                         uint32_t(mq_), got, vb_enter, va,
                         flags == OS_MESG_BLOCK, true);
    }

    // A higher-priority thread may now be runnable; reschedule if so.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return received ? 0 : -1;
}
