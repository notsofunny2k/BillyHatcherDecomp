// Guest time and timed events: VI retrace (59.94 Hz) and the AI DMA
// interrupt that drives AX audio frames (every 5 ms).
#include <windows.h>

#include "audio/audio.h"
#include "hle_irq.h"
#include "hle_time.h"
#include "ppc_runtime.h"

#define TB_PER_SEC 40500000ull
#define VI_PERIOD (TB_PER_SEC * 1001ull / 60000ull)  // NTSC field rate
#define AI_PERIOD (TB_PER_SEC / 200ull)              // 5 ms AX frame
#define VI_DI0_HI 0x2030u                            // display interrupt 0, status bit 0x8000

static uint64_t next_vi, next_ai;

static void raise_retrace(void) {
    ppc_mmio_set16(VI_DI0_HI, (uint16_t)(ppc_mmio_get16(VI_DI0_HI) | 0x8000u));
    hle_irq_raise(IRQ_PI_VI);
}

static void init_clocks(uint64_t now) {
    if (!next_vi) next_vi = now + VI_PERIOD;
    if (!next_ai) next_ai = now + AI_PERIOD;
}

// Raises timed interrupts that are due (cheap; called at interrupt-enable points).
void hle_time_poll(void) {
    uint64_t now = ppc_get_timebase();
    init_clocks(now);
    if (now >= next_vi) {
        next_vi += VI_PERIOD;
        if (next_vi < now) next_vi = now + VI_PERIOD;  // fell behind: don't burst
        raise_retrace();
    }
    int due = 0;
    while (now >= next_ai && due < 8) {
        next_ai += AI_PERIOD;
        due++;
    }
    if (now >= next_ai) next_ai = now + AI_PERIOD;  // far behind: resync
    audio_pump(due);
}

// Nothing can run: wait for the next timed event and raise it.
int hle_time_idle(void) {
    uint64_t now = ppc_get_timebase();
    init_clocks(now);
    uint64_t next = next_vi < next_ai ? next_vi : next_ai;
    if (now < next) {
        uint64_t ms = (next - now) * 1000ull / TB_PER_SEC;
        if (ms) Sleep((DWORD)ms);
        while (ppc_get_timebase() < next) { /* spin the last fraction */ }
    }
    hle_time_poll();
    return 1;
}
