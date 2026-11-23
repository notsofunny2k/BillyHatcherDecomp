// Native replacement for the DSP running the AX microcode.
//
// Every 5 ms the AI DMA interrupt runs __AXOutAiCallback (replaced here):
// the translated AX library syncs its voices into the DSP-side parameter
// blocks (__AXOutNewFrame -> __AXSyncPBs), runs user callbacks (CRI/ADX
// streaming...) and then this mixer plays one frame (160 stereo samples at
// 32 kHz) from those parameter blocks, writing the voice state back exactly
// like the DSP would (state, current address, ADPCM history, envelope).
//
// Not emulated yet: aux A/B buses (reverb), ITD, per-ms parameter updates,
// low-pass filter, 4-tap polyphase SRC (linear interpolation is used).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle_irq.h"
#include "audio.h"
#include "ppc_runtime.h"

#define AX_PB_ARRAY 0x8028FB00u
#define AX_PB_SIZE 0xF4u
#define AX_MAX_VOICES 64
#define AX_OUT_NEW_FRAME 0x801E68DCu  // __AXOutNewFrame
#define SAMPLES 160

// parameter block offsets
enum {
    PB_SRC_SELECT = 0x08, PB_STATE = 0x0E, PB_TYPE = 0x10,
    PB_VL = 0x12, PB_DVL = 0x14, PB_VR = 0x16, PB_DVR = 0x18,
    PB_VE_VOL = 0x64, PB_VE_DELTA = 0x66,
    PB_LOOP = 0x6E, PB_FORMAT = 0x70, PB_LOOP_ADDR = 0x72, PB_END_ADDR = 0x76, PB_CUR_ADDR = 0x7A,
    PB_COEFS = 0x7E, PB_GAIN = 0x9E, PB_PRED_SCALE = 0xA0, PB_YN1 = 0xA2, PB_YN2 = 0xA4,
    PB_RATIO = 0xA6, PB_FRAC = 0xAA, PB_LAST = 0xAC,
    PB_LOOP_PRED_SCALE = 0xB4, PB_LOOP_YN1 = 0xB6, PB_LOOP_YN2 = 0xB8,
};
enum { FMT_ADPCM = 0x00, FMT_PCM16 = 0x0A, FMT_PCM8 = 0x19 };

static unsigned long long voices_mixed, frames, audible_frames;
static int peak;

static uint32_t ld32pb(uint32_t pb, uint32_t off) { return (uint32_t)ld16(pb + off) << 16 | ld16(pb + off + 2); }
static void st32pb(uint32_t pb, uint32_t off, uint32_t v) { st16(pb + off, (uint16_t)(v >> 16)); st16(pb + off + 2, (uint16_t)v); }

static int16_t clamp16(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

static uint8_t aram8(uint32_t a) { return ppc_aram ? ppc_aram[a & 0x00FFFFFFu] : 0; }

typedef struct {
    uint32_t pb, cur, loop_addr, end_addr;
    int fmt, looping, stream, running;
    int pred_scale, yn1, yn2;
    int16_t coefs[16];
} Voice;

// Decodes the next source sample and advances the address.
static int next_sample(Voice* v) {
    if (!v->running) return 0;
    int out = 0;
    switch (v->fmt) {
    case FMT_ADPCM: {
        if ((v->cur & 15) == 0) {  // frame header
            v->pred_scale = aram8(v->cur >> 1);
            v->cur += 2;
        }
        uint8_t b = aram8(v->cur >> 1);
        int nib = (v->cur & 1) ? (b & 15) : (b >> 4);
        if (nib >= 8) nib -= 16;
        int idx = (v->pred_scale >> 4) & 7, scale = 1 << (v->pred_scale & 15);
        int c1 = v->coefs[idx * 2], c2 = v->coefs[idx * 2 + 1];
        int val = ((nib * scale) << 11) + 1024 + c1 * v->yn1 + c2 * v->yn2;
        out = clamp16(val >> 11);
        v->yn2 = v->yn1;
        v->yn1 = out;
        break;
    }
    case FMT_PCM16:
        out = (int16_t)((aram8(v->cur * 2) << 8) | aram8(v->cur * 2 + 1));
        break;
    case FMT_PCM8:
        out = (int8_t)aram8(v->cur) << 8;
        break;
    default:
        v->running = 0;
        return 0;
    }
    // end of sample: loop or stop
    if (v->cur == v->end_addr) {
        if (v->looping) {
            v->cur = v->loop_addr;
            if (v->fmt == FMT_ADPCM && !v->stream) {
                v->pred_scale = ld16(v->pb + PB_LOOP_PRED_SCALE);
                v->yn1 = (int16_t)ld16(v->pb + PB_LOOP_YN1);
                v->yn2 = (int16_t)ld16(v->pb + PB_LOOP_YN2);
            }
        } else {
            v->running = 0;
        }
    } else {
        v->cur++;
    }
    return out;
}

static void mix_voice(uint32_t pb, int* L, int* R) {
    Voice v;
    memset(&v, 0, sizeof(v));
    v.pb = pb;
    v.fmt = ld16(pb + PB_FORMAT);
    v.looping = ld16(pb + PB_LOOP) != 0;
    v.stream = ld16(pb + PB_TYPE) == 1;
    v.loop_addr = ld32pb(pb, PB_LOOP_ADDR);
    v.end_addr = ld32pb(pb, PB_END_ADDR);
    v.cur = ld32pb(pb, PB_CUR_ADDR);
    v.pred_scale = ld16(pb + PB_PRED_SCALE);
    v.yn1 = (int16_t)ld16(pb + PB_YN1);
    v.yn2 = (int16_t)ld16(pb + PB_YN2);
    for (int i = 0; i < 16; i++) v.coefs[i] = (int16_t)ld16(pb + PB_COEFS + 2 * i);
    v.running = 1;

    uint32_t ratio = ld32pb(pb, PB_RATIO);
    if (ld16(pb + PB_SRC_SELECT) == 2 || ratio == 0) ratio = 0x10000;  // no SRC
    uint32_t frac = ld16(pb + PB_FRAC);
    int16_t last[4];
    for (int i = 0; i < 4; i++) last[i] = (int16_t)ld16(pb + PB_LAST + 2 * i);

    int vol = ld16(pb + PB_VE_VOL), dvol = (int16_t)ld16(pb + PB_VE_DELTA);
    int vl = ld16(pb + PB_VL), dvl = (int16_t)ld16(pb + PB_DVL);
    int vr = ld16(pb + PB_VR), dvr = (int16_t)ld16(pb + PB_DVR);

    for (int n = 0; n < SAMPLES; n++) {
        // linear interpolation between the two newest source samples
        int s = last[2] + (int)(((int64_t)(last[3] - last[2]) * (int64_t)frac) >> 16);
        // envelope then bus volumes (1.15 fixed point); 64-bit to avoid overflow
        int64_t e = ((int64_t)s * vol) >> 15;
        if (e > 32767) e = 32767;
        if (e < -32768) e = -32768;
        L[n] += (int)((e * vl) >> 15);
        R[n] += (int)((e * vr) >> 15);
        vol += dvol; if (vol < 0) vol = 0; if (vol > 0xFFFF) vol = 0xFFFF;
        vl += dvl; if (vl < 0) vl = 0; if (vl > 0xFFFF) vl = 0xFFFF;
        vr += dvr; if (vr < 0) vr = 0; if (vr > 0xFFFF) vr = 0xFFFF;
        frac += ratio;
        while (frac >= 0x10000) {
            frac -= 0x10000;
            last[0] = last[1]; last[1] = last[2]; last[2] = last[3];
            last[3] = (int16_t)next_sample(&v);
        }
    }

    // write the DSP-side state back (AX reads state, current address, volume)
    st32pb(pb, PB_CUR_ADDR, v.cur);
    st16(pb + PB_PRED_SCALE, (uint16_t)v.pred_scale);
    st16(pb + PB_YN1, (uint16_t)v.yn1);
    st16(pb + PB_YN2, (uint16_t)v.yn2);
    st16(pb + PB_FRAC, (uint16_t)frac);
    for (int i = 0; i < 4; i++) st16(pb + PB_LAST + 2 * i, (uint16_t)last[i]);
    st16(pb + PB_VE_VOL, (uint16_t)vol);  // AX reads the envelope volume back
    // Bus volumes are not written back: AX re-sends them (with their ramp)
    // whenever they change; writing the ramped value would make it run away.
    (void)vl; (void)vr;
    if (!v.running) st16(pb + PB_STATE, 0);
    voices_mixed++;
}

static void mix_frame(void) {
    int L[SAMPLES] = {0}, R[SAMPLES] = {0};
    for (uint32_t i = 0; i < AX_MAX_VOICES; i++) {
        uint32_t pb = AX_PB_ARRAY + i * AX_PB_SIZE;
        if (ld16(pb + PB_STATE) == 1) mix_voice(pb, L, R);
    }
    short out[SAMPLES * 2];
    int fpeak = 0;
    for (int n = 0; n < SAMPLES; n++) {
        out[n * 2] = clamp16(L[n]);
        out[n * 2 + 1] = clamp16(R[n]);
        int a = out[n * 2] < 0 ? -out[n * 2] : out[n * 2];
        int b = out[n * 2 + 1] < 0 ? -out[n * 2 + 1] : out[n * 2 + 1];
        if (a > fpeak) fpeak = a;
        if (b > fpeak) fpeak = b;
    }
    if (fpeak > 64) audible_frames++;
    if (fpeak > peak) peak = fpeak;
    audio_out_push(out, SAMPLES);
    frames++;
}

static int pending_frames;  // AX frames requested but not produced yet

// AI DMA finished: AX builds the next frame(s), the "DSP" mixes them.
// Several requested frames can be served by one interrupt.
void hle___AXOutAiCallback(PPCContext* ctx) {
    uint32_t saved_lr = ctx->lr, saved_r3 = ctx->r[3];
    int n = pending_frames;
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    pending_frames = 0;
    for (int i = 0; i < n; i++) {
        ctx->r[3] = 0;
        ctx->lr = 0;
        ppc_call_indirect(ctx, AX_OUT_NEW_FRAME);
        mix_frame();
    }
    ctx->lr = saved_lr;
    ctx->r[3] = saved_r3;
}

void ax_mix_stats(void) {
    fprintf(stderr, "ax: %llu frames, %llu voice-frames mixed, %llu audible frames, peak %d\n", frames,
            voices_mixed, audible_frames, peak);
    audio_out_stats();
}

#define TARGET_BUFFERED (SAMPLES * 16)  // ~80 ms queued in the sound card

void audio_pump(int wallclock_due) {
    uint16_t csr = ppc_mmio_get16(0x500A);
    if (!(csr & 0x10)) return;  // AIDINTMSK: AX not running yet
    int need;
    int buffered = audio_out_buffered();
    if (buffered < 0 || frames < 8) {
        need = wallclock_due;  // no device (or warming up): follow the wall clock
    } else {
        need = (TARGET_BUFFERED - buffered) / SAMPLES - pending_frames;
    }
    if (need <= 0) return;
    pending_frames += need;
    // DSP CSR: 0x08 = AIDINT (AI DMA done), raise __OS_INTERRUPT_DSP_AI
    ppc_mmio_set16(0x500A, (uint16_t)(csr | 0x08));
    hle_irq_raise(IRQ_DSP_AI);
}
