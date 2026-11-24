// Native OS alarms (OSSetAlarm / periodic alarms / OSCancelAlarm).
//
// On the console alarms are driven by the decrementer exception. Here the
// active alarms are kept in a native list; when one is due its handler
// (handler(alarm, context)) is queued as a deferred guest call, which runs
// at the next interrupt delivery point. The guest OSAlarm fields (handler,
// fire, period, start) are kept up to date because game code reads them.
#include <stdio.h>
#include <stdlib.h>

#include "hle_alarm.h"
#include "hle_irq.h"
#include "ppc_runtime.h"

// OSAlarm layout
#define AL_HANDLER 0x00
#define AL_FIRE 0x08
#define AL_PERIOD 0x18
#define AL_START 0x20
#define IRQ_CONTEXT (0x80003100u - 0x300u)

typedef struct { uint32_t alarm, handler; uint64_t fire, period; } Alarm;
static Alarm alarms[256];
static int nalarms;
static unsigned long long sets, fired;

static uint64_t ld64g(uint32_t a) { return (uint64_t)ld32(a) << 32 | ld32(a + 4); }
static void st64g(uint32_t a, uint64_t v) { st32(a, (uint32_t)(v >> 32)); st32(a + 4, (uint32_t)v); }

static void remove_alarm(uint32_t alarm) {
    for (int i = 0; i < nalarms; i++)
        if (alarms[i].alarm == alarm) { alarms[i] = alarms[--nalarms]; return; }
}

static void insert(uint32_t alarm, uint64_t fire, uint32_t handler) {
    uint64_t period = ld64g(alarm + AL_PERIOD);
    if (period) {  // periodic: next multiple of period after start
        uint64_t now = ppc_get_timebase(), start = ld64g(alarm + AL_START);
        fire = start;
        if (start < now) fire = start + period * ((now - start) / period + 1);
    }
    remove_alarm(alarm);
    if (nalarms == 256) { fprintf(stderr, "alarm: too many active alarms\n"); return; }
    alarms[nalarms].alarm = alarm;
    alarms[nalarms].handler = handler;
    alarms[nalarms].fire = fire;
    alarms[nalarms].period = period;
    nalarms++;
    st32(alarm + AL_HANDLER, handler);
    st64g(alarm + AL_FIRE, fire);
    sets++;
}

// static void InsertAlarm(OSAlarm* alarm, OSTime fire, OSAlarmHandler handler)
// (64-bit argument in r5:r6, handler in r7)
void hle_InsertAlarm(PPCContext* ctx) {
    insert(ctx->r[3], (uint64_t)ctx->r[5] << 32 | ctx->r[6], ctx->r[7]);
}

// void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler)
void hle_OSSetAlarm(PPCContext* ctx) {
    uint32_t alarm = ctx->r[3];
    uint64_t tick = (uint64_t)ctx->r[5] << 32 | ctx->r[6];
    st64g(alarm + AL_PERIOD, 0);
    insert(alarm, ppc_get_timebase() + tick, ctx->r[7]);
}

void hle_OSCancelAlarm(PPCContext* ctx) {
    uint32_t alarm = ctx->r[3];
    remove_alarm(alarm);
    st32(alarm + AL_HANDLER, 0);
}

// Earliest pending alarm time (UINT64_MAX if none).
uint64_t hle_alarm_next(void) {
    uint64_t n = UINT64_MAX;
    for (int i = 0; i < nalarms; i++)
        if (alarms[i].fire < n) n = alarms[i].fire;
    return n;
}

// Queues the handlers of all due alarms (called from the time poll).
void hle_alarm_poll(uint64_t now) {
    for (int i = 0; i < nalarms;) {
        Alarm a = alarms[i];
        if (a.fire > now) { i++; continue; }
        if (a.period) {  // re-arm before calling, like the OS
            alarms[i].fire = a.fire + a.period * ((now - a.fire) / a.period + 1);
            st64g(a.alarm + AL_FIRE, alarms[i].fire);
            i++;
        } else {
            alarms[i] = alarms[--nalarms];
            st32(a.alarm + AL_HANDLER, 0);
        }
        hle_defer_call(a.handler, a.alarm, IRQ_CONTEXT, 0);
        fired++;
    }
}

void hle_alarm_stats(void) {
    fprintf(stderr, "alarm: %llu set, %llu fired, %d active\n", sets, fired, nalarms);
}
