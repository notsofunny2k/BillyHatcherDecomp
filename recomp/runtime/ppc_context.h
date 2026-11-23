// PowerPC (Gekko) CPU state and memory access helpers used by recompiled code.
#ifndef PPC_CONTEXT_H
#define PPC_CONTEXT_H

#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PPCContext {
    uint32_t r[32];
    double f[32];     // FPR / paired-single slot 0
    double ps1[32];   // paired-single slot 1
    uint8_t cr[8];    // CR fields, bits: LT=8 GT=4 EQ=2 SO=1
    uint32_t lr, ctr;
    uint32_t xer_so, xer_ov, xer_ca;
    uint32_t fpscr;
    uint32_t msr;
    uint32_t spr[1024];  // everything else (GQRs live at spr[912..919])
} PPCContext;

// Guest memory: 24 MB main RAM (mirrored at 0x80000000 / 0xC0000000) plus the
// 16 KB locked L1 cache at 0xE0000000, placed at 0x01800000.
#define PPC_MEM_SIZE 0x01804000u
extern uint8_t* ppc_mem;

static inline uint32_t ppc_xlate(uint32_t a) {
    if ((a >> 28) == 0xE) return 0x01800000u + (a & 0x3FFFu);
    return a & 0x01FFFFFFu;
}
static inline uint8_t* ppc_ptr(uint32_t a) { return ppc_mem + ppc_xlate(a); }

// Hardware registers (0xCC00xxxx) go to the runtime's device layer.
uint32_t ppc_mmio_read(uint32_t a, int size);
void ppc_mmio_write(uint32_t a, uint32_t v, int size);
#define PPC_IS_MMIO(a) __builtin_expect(((a) >> 24) == 0xCC, 0)

static inline uint8_t ld8(uint32_t a) {
    if (PPC_IS_MMIO(a)) return (uint8_t)ppc_mmio_read(a, 1);
    return *ppc_ptr(a);
}
static inline uint16_t ld16(uint32_t a) {
    if (PPC_IS_MMIO(a)) return (uint16_t)ppc_mmio_read(a, 2);
    uint16_t v; memcpy(&v, ppc_ptr(a), 2); return __builtin_bswap16(v);
}
static inline uint32_t ld32(uint32_t a) {
    if (PPC_IS_MMIO(a)) return ppc_mmio_read(a, 4);
    uint32_t v; memcpy(&v, ppc_ptr(a), 4); return __builtin_bswap32(v);
}
static inline uint64_t ld64(uint32_t a) { uint64_t v; memcpy(&v, ppc_ptr(a), 8); return __builtin_bswap64(v); }
static inline void st8(uint32_t a, uint8_t v) {
    if (PPC_IS_MMIO(a)) { ppc_mmio_write(a, v, 1); return; }
    *ppc_ptr(a) = v;
}
static inline void st16(uint32_t a, uint16_t v) {
    if (PPC_IS_MMIO(a)) { ppc_mmio_write(a, v, 2); return; }
    v = __builtin_bswap16(v); memcpy(ppc_ptr(a), &v, 2);
}
static inline void st32(uint32_t a, uint32_t v) {
    if (PPC_IS_MMIO(a)) { ppc_mmio_write(a, v, 4); return; }
    v = __builtin_bswap32(v); memcpy(ppc_ptr(a), &v, 4);
}
static inline void st64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(ppc_ptr(a), &v, 8); }

// Call trace ring buffer (last entered guest functions), for diagnostics.
extern volatile uint32_t ppc_trace_ring[1024];
extern volatile uint32_t ppc_trace_lr[1024];
extern volatile uint32_t ppc_trace_pos;
#define PPC_ENTER(a)                                    \
    do {                                                \
        uint32_t ppc_tp_ = ppc_trace_pos++ & 1023u;     \
        ppc_trace_ring[ppc_tp_] = (a);                  \
        ppc_trace_lr[ppc_tp_] = ctx->lr;                \
    } while (0)

static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static inline uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

static inline float ldf32(uint32_t a) { return u2f(ld32(a)); }
static inline double ldf64(uint32_t a) { return u2d(ld64(a)); }
static inline void stf32(uint32_t a, float v) { st32(a, f2u(v)); }
static inline void stf64(uint32_t a, double v) { st64(a, d2u(v)); }

// Round to single precision (Gekko single ops / paired singles).
static inline double R32(double x) { return (double)(float)x; }

static inline uint32_t ROTL32(uint32_t x, uint32_t n) {
    n &= 31;
    return n ? (x << n) | (x >> (32 - n)) : x;
}

static inline void ppc_set_cr0(PPCContext* ctx, int32_t v) {
    ctx->cr[0] = (uint8_t)((v < 0 ? 8 : v > 0 ? 4 : 2) | (ctx->xer_so & 1));
}
static inline void ppc_cmp_s(PPCContext* ctx, int f, int32_t a, int32_t b) {
    ctx->cr[f] = (uint8_t)((a < b ? 8 : a > b ? 4 : 2) | (ctx->xer_so & 1));
}
static inline void ppc_cmp_u(PPCContext* ctx, int f, uint32_t a, uint32_t b) {
    ctx->cr[f] = (uint8_t)((a < b ? 8 : a > b ? 4 : 2) | (ctx->xer_so & 1));
}
static inline void ppc_fcmp(PPCContext* ctx, int f, double a, double b) {
    uint8_t c = (a != a || b != b) ? 1 : a < b ? 8 : a > b ? 4 : 2;
    ctx->cr[f] = c;
    ctx->fpscr = (ctx->fpscr & ~0xF000u) | ((uint32_t)c << 12);
}
static inline int ppc_get_crbit(PPCContext* ctx, int n) { return (ctx->cr[n >> 2] >> (3 - (n & 3))) & 1; }
static inline void ppc_set_crbit(PPCContext* ctx, int n, int v) {
    uint8_t m = (uint8_t)(1u << (3 - (n & 3)));
    ctx->cr[n >> 2] = (uint8_t)(v ? (ctx->cr[n >> 2] | m) : (ctx->cr[n >> 2] & ~m));
}
static inline uint32_t ppc_get_cr(PPCContext* ctx) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint32_t)(ctx->cr[i] & 0xF) << (28 - 4 * i);
    return v;
}
static inline void ppc_set_cr(PPCContext* ctx, uint32_t v, uint32_t crm) {
    for (int i = 0; i < 8; i++)
        if (crm & (0x80u >> i)) ctx->cr[i] = (uint8_t)((v >> (28 - 4 * i)) & 0xF);
}
static inline uint32_t ppc_get_xer(PPCContext* ctx) {
    return (ctx->xer_so << 31) | (ctx->xer_ov << 30) | (ctx->xer_ca << 29) | (ctx->spr[1] & 0x7F);
}
static inline void ppc_set_xer(PPCContext* ctx, uint32_t v) {
    ctx->xer_so = (v >> 31) & 1; ctx->xer_ov = (v >> 30) & 1; ctx->xer_ca = (v >> 29) & 1;
    ctx->spr[1] = v & 0x7F;
}

// fctiw / fctiwz: result goes to the low word of the FPR.
static inline void ppc_fctiw(PPCContext* ctx, int d, double v, int round_to_zero) {
    int32_t r;
    if (v != v) r = (int32_t)0x80000000;
    else if (v >= 2147483647.0) r = 0x7FFFFFFF;
    else if (v <= -2147483648.0) r = (int32_t)0x80000000;
    else r = (int32_t)(round_to_zero ? trunc(v) : nearbyint(v));
    ctx->f[d] = u2d(0xFFF8000000000000ull | (uint32_t)r);
}

// Paired-single quantized loads/stores (psq_l / psq_st), using GQR[i].
static inline double ppc_dequant(uint32_t raw, int type, int scale, int* size) {
    double s = ldexp(1.0, -scale);
    switch (type) {
    case 4: *size = 1; return (double)(uint8_t)raw * s;
    case 5: *size = 2; return (double)(uint16_t)raw * s;
    case 6: *size = 1; return (double)(int8_t)raw * s;
    case 7: *size = 2; return (double)(int16_t)raw * s;
    default: *size = 4; return (double)u2f(raw);
    }
}
static inline int ppc_sext6(uint32_t v) { return (int)((v & 0x20) ? (v | ~0x3Fu) : v); }

static inline double ppc_psq_ld1(uint32_t ea, int type, int scale) {
    int sz;
    switch (type) {
    case 4: case 6: return ppc_dequant(ld8(ea), type, scale, &sz);
    case 5: case 7: return ppc_dequant(ld16(ea), type, scale, &sz);
    default: return (double)ldf32(ea);
    }
}
static inline int ppc_qsize(int type) { return (type == 4 || type == 6) ? 1 : (type == 5 || type == 7) ? 2 : 4; }

static inline void ppc_psq_l(PPCContext* ctx, int d, uint32_t ea, int w, int i) {
    uint32_t g = ctx->spr[912 + i];
    int type = (g >> 16) & 7, scale = ppc_sext6((g >> 24) & 0x3F);
    ctx->f[d] = ppc_psq_ld1(ea, type, scale);
    ctx->ps1[d] = w ? 1.0 : ppc_psq_ld1(ea + (uint32_t)ppc_qsize(type), type, scale);
}

static inline void ppc_psq_st1(uint32_t ea, double v, int type, int scale) {
    double q = (type == 0 || type > 7 || type < 4) ? v : v * ldexp(1.0, scale);
    switch (type) {
    case 4: st8(ea, (uint8_t)(q < 0 ? 0 : q > 255 ? 255 : q)); break;
    case 5: st16(ea, (uint16_t)(q < 0 ? 0 : q > 65535 ? 65535 : q)); break;
    case 6: st8(ea, (uint8_t)(int8_t)(q < -128 ? -128 : q > 127 ? 127 : q)); break;
    case 7: st16(ea, (uint16_t)(int16_t)(q < -32768 ? -32768 : q > 32767 ? 32767 : q)); break;
    default: stf32(ea, (float)v); break;
    }
}
static inline void ppc_psq_st(PPCContext* ctx, int s, uint32_t ea, int w, int i) {
    uint32_t g = ctx->spr[912 + i];
    int type = g & 7, scale = ppc_sext6((g >> 8) & 0x3F);
    ppc_psq_st1(ea, ctx->f[s], type, scale);
    if (!w) ppc_psq_st1(ea + (uint32_t)ppc_qsize(type), ctx->ps1[s], type, scale);
}

// Provided by the runtime.
typedef void (*PPCFunc)(PPCContext* ctx);
void ppc_call_indirect(PPCContext* ctx, uint32_t addr);
void ppc_unimplemented(PPCContext* ctx, uint32_t addr, uint32_t ins);
void ppc_missing(PPCContext* ctx, uint32_t addr);
void ppc_syscall(PPCContext* ctx);
uint64_t ppc_get_timebase(void);

#ifdef __cplusplus
}
#endif
#endif
