// Native replacement of the SI (serial interface) layer used by PAD.
// Controller state comes from the host (input.c: keyboard + XInput).
#include <string.h>

#include "input.h"
#include "ppc_runtime.h"

#define SI_GC_CONTROLLER 0x09000000u
#define SI_ERROR_NO_RESPONSE 0x00000008u

static uint32_t si_type(uint32_t chan) {
    return (chan < 4 && input_connected((int)chan)) ? SI_GC_CONTROLLER : SI_ERROR_NO_RESPONSE;
}

// Calls a guest callback with up to 3 integer arguments, preserving the
// caller's view of r3 (set afterwards by the HLE function).
static void guest_call3(PPCContext* ctx, uint32_t fn, uint32_t a, uint32_t b, uint32_t c) {
    if (!fn) return;
    uint32_t saved_lr = ctx->lr;
    ctx->r[3] = a; ctx->r[4] = b; ctx->r[5] = c;
    ppc_call_indirect(ctx, fn);
    ctx->lr = saved_lr;
}

void hle_SIBusy(PPCContext* ctx) { ctx->r[3] = 0; }
void hle_SIGetType(PPCContext* ctx) { ctx->r[3] = si_type(ctx->r[3]); }

void hle_SIGetTypeAsync(PPCContext* ctx) {
    uint32_t chan = ctx->r[3], cb = ctx->r[4], type = si_type(chan);
    guest_call3(ctx, cb, chan, type, 0);
    ctx->r[3] = type;
}

// BOOL SITransfer(s32 chan, void* out, u32 outBytes, void* in, u32 inBytes, SICallback cb, OSTime delay)
void hle_SITransfer(PPCContext* ctx) {
    uint32_t chan = ctx->r[3], in = ctx->r[6], in_n = ctx->r[7], cb = ctx->r[8];
    int present = chan < 4 && input_connected((int)chan);
    for (uint32_t i = 0; i < in_n; i++) st8(in + i, 0);
    if (present && in_n >= 3) {  // answer to "get type" (0x00) / "get origin" (0x41)
        st8(in + 0, 0x09); st8(in + 1, 0x00); st8(in + 2, 0x00);
    }
    if (present && in_n >= 10) {  // origin: neutral sticks
        st8(in + 2, 0x80); st8(in + 3, 0x80); st8(in + 4, 0x80); st8(in + 5, 0x80);
    }
    guest_call3(ctx, cb, chan, present ? 0u : SI_ERROR_NO_RESPONSE, 0);
    ctx->r[3] = 1;
}

// BOOL SIGetResponse(s32 chan, void* data): 8 bytes of controller status.
void hle_SIGetResponse(PPCContext* ctx) {
    uint32_t chan = ctx->r[3], data = ctx->r[4];
    uint8_t resp[8] = {0};
    int ok = chan < 4 && input_read((int)chan, resp);
    for (int i = 0; i < 8; i++) st8(data + (uint32_t)i, resp[i]);
    ctx->r[3] = (uint32_t)ok;
}

void hle_SISetSamplingRate(PPCContext* ctx) { (void)ctx; }
void hle_SIRefreshSamplingRate(PPCContext* ctx) { (void)ctx; }
void hle_SIEnablePolling(PPCContext* ctx) { ctx->r[3] = 0; }
void hle_SISetXY(PPCContext* ctx) { ctx->r[3] = 0; }
void hle_SISetCommand(PPCContext* ctx) { (void)ctx; }
void hle_SITransferCommands(PPCContext* ctx) { (void)ctx; }
void hle_SIRegisterPollingHandler(PPCContext* ctx) { ctx->r[3] = 1; }
void hle_SIUnregisterPollingHandler(PPCContext* ctx) { ctx->r[3] = 1; }
