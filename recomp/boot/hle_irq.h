#ifndef HLE_IRQ_H
#define HLE_IRQ_H

#include "ppc_context.h"

// Dolphin OS interrupt numbers (__OSInterrupt)
enum {
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
int hle_irq_deliver(PPCContext* ctx);
int hle_irq_pending(void);
int hle_irq_active(void);

// from hle_thread.c
void hle_thread_reschedule(void);
PPCContext* hle_thread_current_ctx(void);

#endif
