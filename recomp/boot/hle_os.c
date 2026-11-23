// Native (HLE) replacements for SDK / runtime functions.
// Each hle_<Name> replaces the guest function <Name> listed in hle.txt.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ppc_runtime.h"

// ---- debugger / TRK: not present on PC --------------------------------
void hle_InitMetroTRK(PPCContext* ctx) { ctx->r[3] = 0; }
void hle_InitMetroTRK_BBA(PPCContext* ctx) { ctx->r[3] = 0; }
void hle_DBInit(PPCContext* ctx) { (void)ctx; }

// ---- audio DSP bring-up: the PC build mixes audio natively -------------
void hle___OSInitAudioSystem(PPCContext* ctx) { (void)ctx; }
void hle___OSStopAudioSystem(PPCContext* ctx) { (void)ctx; }
// AX: starts the AX microcode task on the DSP and spins until it reports in.
// Audio will be mixed natively; for now AX runs without a DSP (silent).
void hle___AXOutInitDSP(PPCContext* ctx) { (void)ctx; }
// Uploads and starts the first DSP task, then waits for the DSP's mail.
void hle___DSP_boot_task(PPCContext* ctx) { (void)ctx; }
// Calibrates the AI sample-rate converter by timing the hardware counter.
void hle___AI_SRC_INIT(PPCContext* ctx) { (void)ctx; }

// ---- MMU / BAT setup: guest memory is already flat-mapped on PC ---------
void hle_RealMode(PPCContext* ctx) { (void)ctx; }

// ---- guest printf ------------------------------------------------------
// Formats a guest format string with guest varargs (r4.., f1.. per the
// PowerPC EABI, spilling to the stack is not handled: up to 7 int args).
static void guest_format(PPCContext* ctx, uint32_t fmt, int first_gpr, char* out, size_t outsz) {
    int gpr = first_gpr, fpr = 1;
    size_t o = 0;
    char spec[32];
    for (uint32_t p = fmt;; p++) {
        char c = (char)ld8(p);
        if (!c || o + 1 >= outsz) break;
        if (c != '%') { out[o++] = c; continue; }
        size_t s = 0;
        spec[s++] = '%';
        int longs = 0;
        for (;;) {
            c = (char)ld8(++p);
            if (!c) break;
            if (s < sizeof(spec) - 2) spec[s++] = c;
            if (c == 'l') longs++;
            if (strchr("diouxXcspfFeEgGaAn%", c)) break;
        }
        spec[s] = 0;
        char buf[512];
        buf[0] = 0;
        switch (c) {
        case '%': strcpy(buf, "%"); break;
        case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': case 'c':
            if (longs >= 2) {
                if (gpr & 1) gpr++;  // 64-bit values start on an odd register (r3, r5, r7...)
                uint64_t v = gpr <= 9 ? ((uint64_t)ctx->r[gpr] << 32) | ctx->r[gpr + 1] : 0;
                gpr += 2;
                snprintf(buf, sizeof(buf), spec, (long long)v);
            } else {
                uint32_t v = gpr <= 10 ? ctx->r[gpr] : 0;
                gpr++;
                // drop a single 'l': long is 32-bit on the guest
                char sp2[32]; size_t k = 0;
                for (size_t i = 0; spec[i]; i++) if (spec[i] != 'l') sp2[k++] = spec[i];
                sp2[k] = 0;
                snprintf(buf, sizeof(buf), sp2, v);
            }
            break;
        case 'p': snprintf(buf, sizeof(buf), "%08X", gpr <= 10 ? ctx->r[gpr] : 0); gpr++; break;
        case 's': {
            uint32_t a = gpr <= 10 ? ctx->r[gpr] : 0;
            gpr++;
            char str[400];
            size_t k = 0;
            if (a) while (k < sizeof(str) - 1 && ld8(a + (uint32_t)k)) { str[k] = (char)ld8(a + (uint32_t)k); k++; }
            str[k] = 0;
            snprintf(buf, sizeof(buf), spec, str);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double v = fpr <= 8 ? ctx->f[fpr] : 0.0;
            fpr++;
            snprintf(buf, sizeof(buf), spec, v);
            break;
        }
        default: break;
        }
        for (char* b = buf; *b && o + 1 < outsz; b++) out[o++] = *b;
        if (!c) break;
    }
    out[o] = 0;
}

void hle_OSReport(PPCContext* ctx) {
    char buf[2048];
    guest_format(ctx, ctx->r[3], 4, buf, sizeof(buf));
    fputs(buf, stdout);
    fflush(stdout);
}

void hle_OSPanic(PPCContext* ctx) {
    char file[256] = {0}, msg[2048];
    for (uint32_t i = 0; i < sizeof(file) - 1 && ld8(ctx->r[3] + i); i++) file[i] = (char)ld8(ctx->r[3] + i);
    guest_format(ctx, ctx->r[5], 6, msg, sizeof(msg));
    ppc_fatal("OSPanic in %s:%u: %s", file, ctx->r[4], msg);
}
