// Interrupt delivery for emulated devices.
//
// Devices mark an OS interrupt (__OSInterrupt number) pending. It is delivered
// when the guest enables interrupts (OSEnableInterrupts / OSRestoreInterrupts)
// or when the scheduler is idle, by calling the handler that the OS stored in
// its handler table, with the full CPU state saved and restored around it.
#include <stdio.h>
#include <string.h>

#include "hle_irq.h"
#include "hle_time.h"
#include "ppc_runtime.h"

#define MSR_EE 0x8000u
#define OS_INTERRUPT_TABLE 0x80003040u
#define OS_INTERRUPT_MASK 0x800000C4u
#define OS_USER_INTERRUPT_MASK 0x800000C8u
#define IRQ_CONTEXT 0x80003100u - 0x300u  // scratch OSContext for handlers (unused low RAM)

static volatile uint32_t pending;  // bit n = interrupt n pending
static int in_irq;

void hle_irq_raise(int n) { pending |= 1u << n; }
int hle_irq_active(void) { return in_irq; }

static int masked(int n) {
    uint32_t m = ld32(OS_INTERRUPT_MASK) | ld32(OS_USER_INTERRUPT_MASK);
    return (m & (0x80000000u >> n)) != 0;
}

uint32_t hle_find_function(const char* name) {
    for (size_t i = 0; i < ppc_func_table_size; i++)
        if (ppc_func_table[i].name && !strcmp(ppc_func_table[i].name, name)) return ppc_func_table[i].addr;
    return 0;
}

// ---- deferred guest calls ------------------------------------------------
// Native device code that must call back into the game "later" (as hardware
// completion would) queues the call here; it runs at the next delivery point
// like an interrupt handler.
typedef struct { uint32_t fn, r3, r4, r5; } Deferred;
static Deferred dq[64];
static int dq_head, dq_tail;

void hle_defer_call(uint32_t fn, uint32_t r3, uint32_t r4, uint32_t r5) {
    int next = (dq_tail + 1) % 64;
    if (next == dq_head) { fprintf(stderr, "irq: deferred call queue full\n"); return; }
    dq[dq_tail].fn = fn; dq[dq_tail].r3 = r3; dq[dq_tail].r4 = r4; dq[dq_tail].r5 = r5;
    dq_tail = next;
}

static void run_guest(PPCContext* ctx, uint32_t fn, uint32_t r3, uint32_t r4, uint32_t r5) {
    static PPCContext saved;
    memcpy(&saved, ctx, sizeof(saved));
    in_irq = 1;
    ctx->msr &= ~MSR_EE;
    ctx->r[1] -= 0x200;  // stay clear of the interrupted code's red zone
    ctx->r[3] = r3;
    ctx->r[4] = r4;
    ctx->r[5] = r5;
    ctx->lr = 0;
    ppc_call_indirect(ctx, fn);
    in_irq = 0;
    memcpy(ctx, &saved, sizeof(saved));
}

// Runs pending, unmasked interrupts on ctx. Returns how many ran.
int hle_irq_deliver(PPCContext* ctx) {
    if (in_irq) return 0;
    hle_time_poll();
    int ran = 0;
    while (dq_head != dq_tail) {
        Deferred d = dq[dq_head];
        dq_head = (dq_head + 1) % 64;
        run_guest(ctx, d.fn, d.r3, d.r4, d.r5);
        ran++;
    }
    for (int n = 0; n < 32 && pending; n++) {
        if (!(pending & (1u << n)) || masked(n)) continue;
        uint32_t handler = ld32(OS_INTERRUPT_TABLE + 4u * (uint32_t)n);
        pending &= ~(1u << n);
        if (!handler) continue;
        run_guest(ctx, handler, (uint32_t)n, IRQ_CONTEXT, 0);
        ran++;
    }
    if (ran) hle_thread_reschedule();
    return ran;
}

int hle_irq_pending(void) {
    if (dq_head != dq_tail) return 1;
    for (int n = 0; n < 32; n++)
        if ((pending & (1u << n)) && !masked(n) && ld32(OS_INTERRUPT_TABLE + 4u * (uint32_t)n)) return 1;
    return 0;
}

// ---- interrupt enable/disable (replace the MSR-poking OS versions) -------
void hle_OSDisableInterrupts(PPCContext* ctx) {
    uint32_t prev = (ctx->msr & MSR_EE) ? 1 : 0;
    ctx->msr &= ~MSR_EE;
    ctx->r[3] = prev;
}

void hle_OSEnableInterrupts(PPCContext* ctx) {
    uint32_t prev = (ctx->msr & MSR_EE) ? 1 : 0;
    ctx->msr |= MSR_EE;
    hle_irq_deliver(ctx);
    ctx->r[3] = prev;
}

void hle_OSRestoreInterrupts(PPCContext* ctx) {
    uint32_t level = ctx->r[3];
    uint32_t prev = (ctx->msr & MSR_EE) ? 1 : 0;
    if (level) {
        ctx->msr |= MSR_EE;
        hle_irq_deliver(ctx);
    } else {
        ctx->msr &= ~MSR_EE;
    }
    ctx->r[3] = prev;
}
