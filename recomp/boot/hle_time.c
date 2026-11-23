// Guest time and timed events: the VI retrace interrupt (59.94 Hz).
#include <windows.h>

#include "hle_irq.h"
#include "hle_time.h"
#include "ppc_runtime.h"

#define TB_PER_SEC 40500000ull
#define VI_PERIOD (TB_PER_SEC * 1001ull / 60000ull)  // NTSC field rate
#define VI_DI0_HI 0x2030u                            // display interrupt 0, status bit 0x8000

static uint64_t next_vi;

static void raise_retrace(void) {
    ppc_mmio_set16(VI_DI0_HI, (uint16_t)(ppc_mmio_get16(VI_DI0_HI) | 0x8000u));
    hle_irq_raise(IRQ_PI_VI);
}

// Called often (interrupt enable points): raise the retrace when it's due.
void hle_time_poll(void) {
    uint64_t now = ppc_get_timebase();
    if (!next_vi) next_vi = now + VI_PERIOD;
    if (now >= next_vi) {
        next_vi += VI_PERIOD;
        if (next_vi < now) next_vi = now + VI_PERIOD;  // fell behind: don't burst
        raise_retrace();
    }
}

int hle_time_idle(void) {
    uint64_t now = ppc_get_timebase();
    if (!next_vi) next_vi = now + VI_PERIOD;
    if (now < next_vi) {
        uint64_t ms = (next_vi - now) * 1000ull / TB_PER_SEC;
        if (ms) Sleep((DWORD)ms);
        while (ppc_get_timebase() < next_vi) { /* spin the last fraction */ }
    }
    next_vi += VI_PERIOD;
    raise_retrace();
    return 1;
}
