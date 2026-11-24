#ifndef HLE_IRQ_H
#define HLE_IRQ_H

#include "ppc_context.h"

// Dolphin OS interrupt numbers (__OSInterrupt)
enum {
    IRQ_EXI0_EXI = 9,
    IRQ_EXI0_TC = 10,
    IRQ_EXI0_EXT = 11,
    IRQ_DSP_AI = 5,
    IRQ_DSP_ARAM = 6,
    IRQ_DSP_DSP = 7,
    IRQ_AI_AI = 8,
    IRQ_PI_CP = 17,
    IRQ_PI_PE_TOKEN = 18,
    IRQ_PI_PE_FINISH = 19,
    IRQ_PI_SI = 20,
    IRQ_PI_DI = 21,
    IRQ_PI_VI = 24,
};

void hle_irq_raise(int n);
// Calls guest function fn(r3, r4, r5) at the next interrupt delivery point.
void hle_defer_call(uint32_t fn, uint32_t r3, uint32_t r4, uint32_t r5);
// Address of a translated guest function by name (0 if unknown).
uint32_t hle_find_function(const char* name);
int hle_irq_deliver(PPCContext* ctx);
int hle_irq_pending(void);
int hle_irq_active(void);

// from hle_thread.c
void hle_thread_reschedule(void);
PPCContext* hle_thread_current_ctx(void);

#endif
