// GX texture decoding (tiled formats -> RGBA8) with a small GL texture cache.
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <GL/gl.h>

#include "gx_internal.h"
#include "ppc_runtime.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

enum { TF_I4 = 0, TF_I8 = 1, TF_IA4 = 2, TF_IA8 = 3, TF_RGB565 = 4, TF_RGB5A3 = 5, TF_RGBA8 = 6,
       TF_C4 = 8, TF_C8 = 9, TF_C14X2 = 10, TF_CMPR = 14 };

static inline uint8_t ex4(uint32_t v) { return (uint8_t)(v * 17); }
static inline uint8_t ex5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static inline uint8_t ex6(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }
static inline uint8_t ex3(uint32_t v) { return (uint8_t)((v << 5) | (v << 2) | (v >> 1)); }

static void rgb565(uint16_t c, uint8_t* o) { o[0] = ex5(c >> 11); o[1] = ex6((c >> 5) & 63); o[2] = ex5(c & 31); o[3] = 255; }
static void rgb5a3(uint16_t c, uint8_t* o) {
    if (c & 0x8000) { o[0] = ex5((c >> 10) & 31); o[1] = ex5((c >> 5) & 31); o[2] = ex5(c & 31); o[3] = 255; }
    else { o[0] = ex4((c >> 8) & 15); o[1] = ex4((c >> 4) & 15); o[2] = ex4(c & 15); o[3] = ex3((c >> 12) & 7); }
}
static void ia8(uint16_t c, uint8_t* o) { o[0] = o[1] = o[2] = (uint8_t)c; o[3] = (uint8_t)(c >> 8); }

static void palette(uint16_t c, int tlut_fmt, uint8_t* o) {
    if (tlut_fmt == 0) ia8(c, o);
    else if (tlut_fmt == 1) rgb565(c, o);
    else rgb5a3(c, o);
}

static int block_w(int fmt) { return (fmt == TF_I4 || fmt == TF_C4 || fmt == TF_CMPR) ? 8 : (fmt == TF_RGBA8 || fmt == TF_IA8 || fmt == TF_RGB565 || fmt == TF_RGB5A3 || fmt == TF_C14X2) ? 4 : 8; }
static int block_h(int fmt) { return (fmt == TF_I4 || fmt == TF_C4 || fmt == TF_I8 || fmt == TF_IA4 || fmt == TF_C8 || fmt == TF_CMPR) ? 8 : 4; }

static void decode_cmpr_sub(const uint8_t* s, uint8_t* out, int stride, int x0, int y0, int w, int h) {
    uint16_t c0 = (uint16_t)(s[0] << 8 | s[1]), c1 = (uint16_t)(s[2] << 8 | s[3]);
    uint8_t pal[4][4];
    rgb565(c0, pal[0]); rgb565(c1, pal[1]);
    for (int i = 0; i < 3; i++) {
        if (c0 > c1) {
            pal[2][i] = (uint8_t)((2 * pal[0][i] + pal[1][i]) / 3);
            pal[3][i] = (uint8_t)((pal[0][i] + 2 * pal[1][i]) / 3);
        } else {
            pal[2][i] = (uint8_t)((pal[0][i] + pal[1][i]) / 2);
            pal[3][i] = 0;
        }
    }
    pal[2][3] = 255;
    pal[3][3] = c0 > c1 ? 255 : 0;
    for (int y = 0; y < 4; y++) {
        uint8_t bits = s[4 + y];
        for (int x = 0; x < 4; x++) {
            int idx = (bits >> (6 - 2 * x)) & 3;
            int px = x0 + x, py = y0 + y;
            if (px < w && py < h) memcpy(out + ((size_t)py * stride + px) * 4, pal[idx], 4);
        }
    }
}

static void decode(uint32_t addr, int w, int h, int fmt, const uint8_t* tlut, int tlut_fmt, uint8_t* out) {
    const uint8_t* src = ppc_ptr(addr);
    int bw = block_w(fmt), bh = block_h(fmt);
    size_t p = 0;
    for (int by = 0; by < h; by += bh)
        for (int bx = 0; bx < w; bx += bw) {
            if (fmt == TF_CMPR) {
                for (int sb = 0; sb < 4; sb++, p += 8)
                    decode_cmpr_sub(src + p, out, w, bx + (sb & 1) * 4, by + (sb >> 1) * 4, w, h);
                continue;
            }
            if (fmt == TF_RGBA8) {  // AR plane (32 bytes) then GB plane (32 bytes)
                for (int i = 0; i < 16; i++) {
                    int x = bx + (i & 3), y = by + (i >> 2);
                    if (x < w && y < h) {
                        uint8_t* o = out + ((size_t)y * w + x) * 4;
                        o[3] = src[p + i * 2]; o[0] = src[p + i * 2 + 1];
                        o[1] = src[p + 32 + i * 2]; o[2] = src[p + 32 + i * 2 + 1];
                    }
                }
                p += 64;
                continue;
            }
            if (fmt == TF_I4 || fmt == TF_C4) {  // 8x8 block, two pixels per byte
                for (int y = by; y < by + 8; y++)
                    for (int x = bx; x < bx + 8; x += 2) {
                        uint8_t b = src[p++];
                        for (int k = 0; k < 2; k++) {
                            uint32_t v = k ? (b & 15u) : (b >> 4u);
                            uint8_t px[4];
                            if (fmt == TF_I4) px[0] = px[1] = px[2] = px[3] = ex4(v);
                            else palette((uint16_t)(tlut[v * 2] << 8 | tlut[v * 2 + 1]), tlut_fmt, px);
                            if (x + k < w && y < h) memcpy(out + ((size_t)y * w + x + k) * 4, px, 4);
                        }
                    }
                continue;
            }
            for (int y = by; y < by + bh; y++)
                for (int x = bx; x < bx + bw; x++) {
                    uint8_t px[4];
                    switch (fmt) {
                    case TF_I8: px[0] = px[1] = px[2] = px[3] = src[p++]; break;
                    case TF_C8: {
                        uint8_t v = src[p++];
                        palette((uint16_t)(tlut[v * 2] << 8 | tlut[v * 2 + 1]), tlut_fmt, px);
                        break;
                    }
                    case TF_IA4: { uint8_t b = src[p++]; px[0] = px[1] = px[2] = ex4(b & 15u); px[3] = ex4(b >> 4); break; }
                    case TF_IA8: ia8((uint16_t)(src[p] << 8 | src[p + 1]), px); p += 2; break;
                    case TF_RGB565: rgb565((uint16_t)(src[p] << 8 | src[p + 1]), px); p += 2; break;
                    case TF_RGB5A3: rgb5a3((uint16_t)(src[p] << 8 | src[p + 1]), px); p += 2; break;
                    case TF_C14X2: {
                        uint32_t v = (uint32_t)(src[p] << 8 | src[p + 1]) & 0x3FFF;
                        palette((uint16_t)(tlut[(v & 255) * 2] << 8 | tlut[(v & 255) * 2 + 1]), tlut_fmt, px);
                        p += 2;
                        break;
                    }
                    default: px[0] = 255; px[1] = 0; px[2] = 255; px[3] = 255; p++; break;
                    }
                    if (x < w && y < h) memcpy(out + ((size_t)y * w + x) * 4, px, 4);
                }
        }
}

typedef struct {
    uint32_t addr, key2, hash;
    unsigned id;
    int used;
} TexEntry;

#define TEX_CACHE 512
static TexEntry cache[TEX_CACHE];
static int frame_counter;

static uint32_t quick_hash(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    size_t step = n > 4096 ? n / 1024 : 1;
    for (size_t i = 0; i < n; i += step) h = (h ^ p[i]) * 16777619u;
    return h;
}

static size_t tex_bytes(int w, int h, int fmt) {
    int bw = block_w(fmt), bh = block_h(fmt);
    size_t blocks = (size_t)((w + bw - 1) / bw) * (size_t)((h + bh - 1) / bh);
    return blocks * 32 * (fmt == TF_RGBA8 ? 2 : 1);
}

void gx_tex_invalidate(void) { frame_counter++; }

unsigned gx_tex_get(uint32_t addr, int w, int h, int fmt, uint32_t tlut_tmem, int tlut_fmt, const uint8_t* tmem) {
    size_t n = tex_bytes(w, h, fmt);
    uint32_t key2 = (uint32_t)w | (uint32_t)h << 11 | (uint32_t)fmt << 22 | (uint32_t)tlut_fmt << 27;
    const uint8_t* tlut = tmem + (tlut_tmem & 0x7FFFF);
    uint32_t hash = quick_hash(ppc_ptr(addr), n);
    if (fmt == TF_C4 || fmt == TF_C8 || fmt == TF_C14X2) hash ^= quick_hash(tlut, fmt == TF_C4 ? 32 : 512) * 31u;
    int slot = (int)((addr >> 5) * 2654435761u % TEX_CACHE);
    for (int i = 0; i < 8; i++) {
        TexEntry* e = &cache[(slot + i) % TEX_CACHE];
        if (e->id && e->addr == addr && e->key2 == key2) {
            if (e->hash == hash) return e->id;
            slot = (slot + i) % TEX_CACHE;
            goto upload;
        }
    }
    for (int i = 0; i < 8; i++) {
        TexEntry* e = &cache[(slot + i) % TEX_CACHE];
        if (!e->id) { slot = (slot + i) % TEX_CACHE; goto upload; }
    }
upload : {
    TexEntry* e = &cache[slot];
    if (!e->id) glGenTextures(1, &e->id);
    e->addr = addr; e->key2 = key2; e->hash = hash;
    uint8_t* rgba = (uint8_t*)malloc((size_t)w * h * 4);
    decode(addr, w, h, fmt, tlut, tlut_fmt, rgba);
    glBindTexture(GL_TEXTURE_2D, e->id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);
    return e->id;
}
}
