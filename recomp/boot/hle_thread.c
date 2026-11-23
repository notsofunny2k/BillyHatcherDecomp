// Native thread scheduler replacing the Dolphin OS one.
//
// Each guest OSThread runs on its own Windows fiber with its own PPCContext,
// so switching threads switches both the guest register state and the host
// stack of the translated code. Scheduling follows the OS rules: the runnable
// thread with the lowest priority value runs; a running thread keeps the CPU
// until it blocks, yields, or a higher-priority thread becomes ready.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "ppc_runtime.h"
#include "hle_irq.h"
#include "hle_time.h"

enum { TS_READY = 1, TS_RUNNING = 2, TS_WAITING = 4, TS_MORIBUND = 8 };

// OSThread field offsets (Dolphin SDK)
#define OT_STATE 0x2C8
#define OT_ATTR 0x2CA
#define OT_SUSPEND 0x2CC
#define OT_PRIORITY 0x2D0
#define OT_BASE 0x2D4
#define OT_VAL 0x2D8
#define OT_QUEUE 0x2DC
#define OT_QUEUE_JOIN 0x2E8
#define OT_MUTEX 0x2F0
#define OT_QUEUE_MUTEX 0x2F4
#define OT_STACK_BASE 0x304
#define OT_STACK_END 0x308
#define OS_CURRENT_THREAD 0x800000E4u

typedef struct GThread {
    uint32_t addr;        // guest OSThread*
    PPCContext* ctx;
    void* fiber;
    uint32_t entry, param;
    int state, suspend, priority;
    uint32_t waitq;       // guest OSThreadQueue* it sleeps on
    uint64_t seq;         // FIFO order among equal priorities
    int started;
} GThread;

static GThread* threads[256];
static int nthreads;
static GThread* cur;
static int sched_disabled;
static uint64_t seq_counter;

static GThread* find(uint32_t addr) {
    for (int i = 0; i < nthreads; i++)
        if (threads[i]->addr == addr) return threads[i];
    return NULL;
}

static void sync_guest(GThread* t) {
    st16(t->addr + OT_STATE, (uint16_t)t->state);
    st32(t->addr + OT_SUSPEND, (uint32_t)t->suspend);
    st32(t->addr + OT_PRIORITY, (uint32_t)t->priority);
    st32(t->addr + OT_QUEUE, t->waitq);
}

// The boot thread: whatever __OSThreadInit made current, on the main fiber.
static void ensure_main(PPCContext* ctx) {
    if (cur) return;
    GThread* t = (GThread*)calloc(1, sizeof(GThread));
    t->addr = ld32(OS_CURRENT_THREAD);
    t->ctx = ctx;
    t->fiber = ConvertThreadToFiber(NULL);
    t->state = TS_RUNNING;
    t->priority = (int32_t)ld32(t->addr + OT_PRIORITY);
    t->started = 1;
    threads[nthreads++] = t;
    cur = t;
}

static int runnable(GThread* t) {
    return (t->state == TS_READY || t->state == TS_RUNNING) && t->suspend <= 0;
}

static GThread* pick(void) {
    GThread* best = NULL;
    for (int i = 0; i < nthreads; i++) {
        GThread* t = threads[i];
        if (!runnable(t)) continue;
        if (!best || t->priority < best->priority ||
            (t->priority == best->priority && t->seq < best->seq))
            best = t;
    }
    // the running thread keeps the CPU against equal-priority threads
    if (best && cur && runnable(cur) && cur != best && cur->priority <= best->priority) best = cur;
    return best;
}

void hle_thread_dump(void) {
    fprintf(stderr, "--- guest threads ---\n");
    for (int i = 0; i < nthreads; i++) {
        GThread* t = threads[i];
        fprintf(stderr, "  %c %08X prio %2d state %d suspend %d waitq %08X entry %08X %s\n",
                t == cur ? '*' : ' ', t->addr, t->priority, t->state, t->suspend, t->waitq, t->entry,
                t->entry ? ppc_func_name(t->entry) : "(boot)");
    }
}

static void WINAPI fiber_main(void* p);

static void switch_to(GThread* t) {
    if (t == cur) return;
    if (cur && cur->state == TS_RUNNING) { cur->state = TS_READY; sync_guest(cur); }
    GThread* prev = cur;
    cur = t;
    t->state = TS_RUNNING;
    sync_guest(t);
    st32(OS_CURRENT_THREAD, t->addr);
    if (!t->fiber) t->fiber = CreateFiber(256 * 1024, fiber_main, t);
    (void)prev;
    SwitchToFiber(t->fiber);
}

// Pick the next thread; if nothing can run, let time pass (alarms, VI
// retrace...) until something wakes up.
static void reschedule(void) {
    if (hle_irq_active()) return;  // switch after the interrupt handler returns
    if (sched_disabled > 0 && cur && runnable(cur)) return;
    for (;;) {
        GThread* best = pick();
        if (best) { switch_to(best); return; }
        // idle: pending interrupts first, then let time pass
        if (cur && hle_irq_pending() && hle_irq_deliver(cur->ctx)) continue;
        if (!hle_time_idle()) {
            fprintf(stderr, "sched: deadlock, no runnable guest thread and no pending events\n");
            hle_thread_dump();
            ppc_fatal("deadlock");
        }
    }
}

void hle_thread_reschedule(void) { if (cur) reschedule(); }
PPCContext* hle_thread_current_ctx(void) { return cur ? cur->ctx : NULL; }
int hle_thread_count(void) { return nthreads; }

static void exit_current(uint32_t val);

static void WINAPI fiber_main(void* p) {
    GThread* t = (GThread*)p;
    PPCContext* c = t->ctx;
    ppc_call_indirect(c, t->entry);
    exit_current(c->r[3]);
}

static void wake_queue(uint32_t queue) {
    for (int i = 0; i < nthreads; i++) {
        GThread* t = threads[i];
        if (t->state == TS_WAITING && t->waitq == queue) {
            t->state = TS_READY;
            t->waitq = 0;
            t->seq = ++seq_counter;
            sync_guest(t);
        }
    }
}

static void exit_current(uint32_t val) {
    cur->state = TS_MORIBUND;
    st32(cur->addr + OT_VAL, val);
    sync_guest(cur);
    wake_queue(cur->addr + OT_QUEUE_JOIN);
    reschedule();
    ppc_fatal("exited thread %08X was resumed", cur->addr);
}

// BOOL OSCreateThread(OSThread*, void* (*func)(void*), void* param,
//                     void* stack, u32 stackSize, OSPriority prio, u16 attr)
void hle_OSCreateThread(PPCContext* ctx) {
    ensure_main(ctx);
    uint32_t th = ctx->r[3], func = ctx->r[4], param = ctx->r[5], stack = ctx->r[6];
    uint32_t size = ctx->r[7], prio = ctx->r[8], attr = ctx->r[9];
    if ((int32_t)prio < 0 || prio > 31) { ctx->r[3] = 0; return; }
    GThread* t = find(th);
    if (!t) {
        t = (GThread*)calloc(1, sizeof(GThread));
        threads[nthreads++] = t;
        t->addr = th;
    } else if (t->fiber && t != cur) {
        DeleteFiber(t->fiber);
        t->fiber = NULL;
    }
    t->entry = func;
    t->param = param;
    t->state = TS_READY;
    t->suspend = 1;
    t->priority = (int)prio;
    t->waitq = 0;
    t->seq = ++seq_counter;
    if (!t->ctx) t->ctx = (PPCContext*)malloc(sizeof(PPCContext));
    PPCContext* c = t->ctx;
    memset(c, 0, sizeof(*c));
    c->r[1] = (stack - 8) & ~7u;
    st32(c->r[1], 0);
    c->r[2] = ctx->r[2];
    c->r[13] = ctx->r[13];
    c->r[3] = param;
    c->msr = ctx->msr;
    c->fpscr = ctx->fpscr;
    memcpy(&c->spr[912], &ctx->spr[912], 8 * sizeof(uint32_t));  // GQRs

    // guest-visible fields
    for (uint32_t i = 0; i < 0x2C8; i += 4) st32(th + i, 0);
    st16(th + OT_ATTR, (uint16_t)attr);
    st32(th + OT_BASE, prio);
    st32(th + OT_VAL, 0);
    st32(th + OT_QUEUE_JOIN, 0); st32(th + OT_QUEUE_JOIN + 4, 0);
    st32(th + OT_MUTEX, 0);
    st32(th + OT_QUEUE_MUTEX, 0); st32(th + OT_QUEUE_MUTEX + 4, 0);
    st32(th + OT_STACK_BASE, stack);
    st32(th + OT_STACK_END, stack - size);
    st32(stack - size, 0xDEADBABEu);
    sync_guest(t);
    ctx->r[3] = 1;
}

void hle_OSResumeThread(PPCContext* ctx) {
    ensure_main(ctx);
    GThread* t = find(ctx->r[3]);
    if (!t) { ctx->r[3] = 0; return; }
    int prev = t->suspend;
    if (t->suspend > 0) t->suspend--;
    if (prev == 1) t->seq = ++seq_counter;
    sync_guest(t);
    ctx->r[3] = (uint32_t)prev;
    reschedule();
    ctx->r[3] = (uint32_t)prev;
}

void hle_OSSuspendThread(PPCContext* ctx) {
    ensure_main(ctx);
    GThread* t = find(ctx->r[3]);
    if (!t) { ctx->r[3] = 0; return; }
    int prev = t->suspend++;
    sync_guest(t);
    if (t == cur) reschedule();
    ctx->r[3] = (uint32_t)prev;
}

void hle_OSSleepThread(PPCContext* ctx) {
    ensure_main(ctx);
    cur->state = TS_WAITING;
    cur->waitq = ctx->r[3];
    sync_guest(cur);
    reschedule();
}

void hle_OSWakeupThread(PPCContext* ctx) {
    ensure_main(ctx);
    wake_queue(ctx->r[3]);
    reschedule();
}

void hle_OSExitThread(PPCContext* ctx) {
    ensure_main(ctx);
    exit_current(ctx->r[3]);
}

void hle_OSCancelThread(PPCContext* ctx) {
    ensure_main(ctx);
    GThread* t = find(ctx->r[3]);
    if (!t) return;
    if (t == cur) exit_current(0);
    t->state = TS_MORIBUND;
    t->waitq = 0;
    sync_guest(t);
    wake_queue(t->addr + OT_QUEUE_JOIN);
    reschedule();
}

void hle_OSSetThreadPriority(PPCContext* ctx) {
    ensure_main(ctx);
    GThread* t = find(ctx->r[3]);
    uint32_t prio = ctx->r[4];
    if (!t || prio > 31) { ctx->r[3] = 0; return; }
    t->priority = (int)prio;
    st32(t->addr + OT_BASE, prio);
    sync_guest(t);
    reschedule();
    ctx->r[3] = 1;
}

void hle_OSDisableScheduler(PPCContext* ctx) { ensure_main(ctx); ctx->r[3] = (uint32_t)sched_disabled++; }
void hle_OSEnableScheduler(PPCContext* ctx) {
    ensure_main(ctx);
    ctx->r[3] = (uint32_t)sched_disabled;
    if (sched_disabled > 0) sched_disabled--;
}

void hle___OSReschedule(PPCContext* ctx) { ensure_main(ctx); reschedule(); }

// SelectThread(BOOL yield): only reached through translated OS code.
void hle_SelectThread(PPCContext* ctx) {
    ensure_main(ctx);
    if (ctx->r[3] && cur) cur->seq = ++seq_counter;  // yield behind equals
    GThread* save = cur;
    if (ctx->r[3] && cur && runnable(cur)) {
        // let an equal-priority thread run
        for (int i = 0; i < nthreads; i++) {
            GThread* t = threads[i];
            if (t != cur && runnable(t) && t->priority <= cur->priority) { switch_to(t); break; }
        }
    } else {
        reschedule();
    }
    (void)save;
}

void hle_OSSaveContext(PPCContext* ctx) { (void)ctx; ppc_fatal("OSSaveContext reached (threads are native)"); }
void hle_OSLoadContext(PPCContext* ctx) { (void)ctx; ppc_fatal("OSLoadContext reached (threads are native)"); }
