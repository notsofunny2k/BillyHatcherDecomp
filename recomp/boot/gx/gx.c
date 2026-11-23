// GX command processor: consumes the write-gather pipe stream (0xCC008000),
// tracks CP / XF / BP register state and draws primitives with OpenGL.
// First version: fixed-function approximation (vertex color x texture 0),
// no lighting, no full TEV, no render-to-texture.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <GL/gl.h>

#include "../hle_irq.h"
#include "gx_internal.h"
#include "ppc_runtime.h"

// ---------------------------------------------------------------- state
static uint32_t cp[256];
static uint32_t xf_mem[0x1100];  // matrices 0x0000-0x04FF, regs 0x1000-0x10FF
static uint32_t bp[256];
static uint8_t* tmem;            // 1 MB texture memory (TLUTs live here)

static uint8_t fifo[1 << 20];
static size_t fifo_len;

static struct {
    unsigned long long cmds, draws, verts, frames, unknown;
} st;

static int gx_ready;

static float xf_f(uint32_t a) { float f; uint32_t v = xf_mem[a & 0x10FF]; memcpy(&f, &v, 4); return f; }

// ------------------------------------------------------------ helpers
static uint32_t rd8(const uint8_t* p) { return p[0]; }
static uint32_t rd16(const uint8_t* p) { return (uint32_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static float rdf(const uint8_t* p) { uint32_t v = rd32(p); float f; memcpy(&f, &v, 4); return f; }

// ------------------------------------------------------- vertex format
typedef struct {
    int pmidx, tmidx[8];
    int pos, nrm, col[2], tex[8];        // 0 none, 1 direct, 2 idx8, 3 idx16
    int pos_cnt, pos_fmt, pos_shift;
    int nrm_cnt, nrm_fmt;
    int col_cnt[2], col_fmt[2];
    int tex_cnt[8], tex_fmt[8], tex_shift[8];
} VtxFmt;

static void get_fmt(int vat, VtxFmt* f) {
    uint32_t lo = cp[0x50], hi = cp[0x60];
    uint32_t a = cp[0x70 + vat], b = cp[0x80 + vat], c = cp[0x90 + vat];
    memset(f, 0, sizeof(*f));
    f->pmidx = lo & 1;
    for (int i = 0; i < 8; i++) f->tmidx[i] = (lo >> (1 + i)) & 1;
    f->pos = (lo >> 9) & 3;
    f->nrm = (lo >> 11) & 3;
    f->col[0] = (lo >> 13) & 3;
    f->col[1] = (lo >> 15) & 3;
    for (int i = 0; i < 8; i++) f->tex[i] = (hi >> (2 * i)) & 3;
    f->pos_cnt = a & 1; f->pos_fmt = (a >> 1) & 7; f->pos_shift = (a >> 4) & 31;
    f->nrm_cnt = (a >> 9) & 1; f->nrm_fmt = (a >> 10) & 7;
    f->col_cnt[0] = (a >> 13) & 1; f->col_fmt[0] = (a >> 14) & 7;
    f->col_cnt[1] = (a >> 17) & 1; f->col_fmt[1] = (a >> 18) & 7;
    f->tex_cnt[0] = (a >> 21) & 1; f->tex_fmt[0] = (a >> 22) & 7; f->tex_shift[0] = (a >> 25) & 31;
    f->tex_cnt[1] = b & 1; f->tex_fmt[1] = (b >> 1) & 7; f->tex_shift[1] = (b >> 4) & 31;
    f->tex_cnt[2] = (b >> 9) & 1; f->tex_fmt[2] = (b >> 10) & 7; f->tex_shift[2] = (b >> 13) & 31;
    f->tex_cnt[3] = (b >> 18) & 1; f->tex_fmt[3] = (b >> 19) & 7; f->tex_shift[3] = (b >> 22) & 31;
    f->tex_cnt[4] = (b >> 27) & 1; f->tex_fmt[4] = (b >> 28) & 7; f->tex_shift[4] = c & 31;
    f->tex_cnt[5] = (c >> 5) & 1; f->tex_fmt[5] = (c >> 6) & 7; f->tex_shift[5] = (c >> 9) & 31;
    f->tex_cnt[6] = (c >> 14) & 1; f->tex_fmt[6] = (c >> 15) & 7; f->tex_shift[6] = (c >> 18) & 31;
    f->tex_cnt[7] = (c >> 23) & 1; f->tex_fmt[7] = (c >> 24) & 7; f->tex_shift[7] = (c >> 27) & 31;
}

static int comp_size(int fmt) { return fmt <= 1 ? 1 : fmt <= 3 ? 2 : 4; }
static int col_size(int fmt) { static const int s[8] = {2, 3, 4, 2, 3, 4, 4, 4}; return s[fmt & 7]; }
static int idx_size(int mode) { return mode == 2 ? 1 : mode == 3 ? 2 : 0; }

static int vtx_size(const VtxFmt* f) {
    int n = f->pmidx;
    for (int i = 0; i < 8; i++) n += f->tmidx[i];
    if (f->pos == 1) n += comp_size(f->pos_fmt) * (f->pos_cnt ? 3 : 2);
    else n += idx_size(f->pos);
    if (f->nrm == 1) n += comp_size(f->nrm_fmt) * 3 * (f->nrm_cnt ? 3 : 1);
    else n += idx_size(f->nrm) * ((f->nrm && f->nrm_cnt && (cp[0x70] >> 31)) ? 3 : 1);
    for (int i = 0; i < 2; i++) n += f->col[i] == 1 ? col_size(f->col_fmt[i]) : idx_size(f->col[i]);
    for (int i = 0; i < 8; i++)
        n += f->tex[i] == 1 ? comp_size(f->tex_fmt[i]) * (f->tex_cnt[i] ? 2 : 1) : idx_size(f->tex[i]);
    return n;
}

static float read_comp(const uint8_t* p, int fmt, int shift) {
    float s = 1.0f / (float)(1u << shift);
    switch (fmt) {
    case 0: return (float)rd8(p) * s;
    case 1: return (float)(int8_t)rd8(p) * s;
    case 2: return (float)rd16(p) * s;
    case 3: return (float)(int16_t)rd16(p) * s;
    default: return rdf(p);
    }
}

static void read_color(const uint8_t* p, int fmt, float* o) {
    uint32_t v;
    switch (fmt) {
    case 0: v = rd16(p); o[0] = ((v >> 11) & 31) / 31.f; o[1] = ((v >> 5) & 63) / 63.f; o[2] = (v & 31) / 31.f; o[3] = 1; break;
    case 1: case 2: o[0] = p[0] / 255.f; o[1] = p[1] / 255.f; o[2] = p[2] / 255.f; o[3] = 1; break;
    case 3: v = rd16(p); o[0] = ((v >> 12) & 15) / 15.f; o[1] = ((v >> 8) & 15) / 15.f; o[2] = ((v >> 4) & 15) / 15.f; o[3] = (v & 15) / 15.f; break;
    case 4: v = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        o[0] = ((v >> 18) & 63) / 63.f; o[1] = ((v >> 12) & 63) / 63.f; o[2] = ((v >> 6) & 63) / 63.f; o[3] = (v & 63) / 63.f; break;
    default: o[0] = p[0] / 255.f; o[1] = p[1] / 255.f; o[2] = p[2] / 255.f; o[3] = p[3] / 255.f; break;
    }
}

// pointer to element `idx` of indexed array `arr` (CP 0xA0+/0xB0+)
static const uint8_t* array_ptr(int arr, uint32_t idx) {
    return ppc_ptr(0x80000000u | ((cp[0xA0 + arr] + idx * (cp[0xB0 + arr] & 0xFF)) & 0x01FFFFFFu));
}

typedef struct { float pos[3], col[4], tex[2]; int pm; int has_col, has_tex; } Vtx;

static const uint8_t* attr(const uint8_t** pp, int mode, int arr) {
    const uint8_t* p = *pp;
    if (mode == 1) return p;  // caller advances
    uint32_t idx = mode == 2 ? rd8(p) : rd16(p);
    *pp += mode == 2 ? 1 : 2;
    return array_ptr(arr, idx);
}

static const uint8_t* decode_vertex(const uint8_t* p, const VtxFmt* f, Vtx* v) {
    v->pm = (int)(cp[0x30] & 63);
    if (f->pmidx) v->pm = (int)rd8(p++);
    for (int i = 0; i < 8; i++) if (f->tmidx[i]) p++;
    // position
    {
        int n = f->pos_cnt ? 3 : 2, cs = comp_size(f->pos_fmt);
        const uint8_t* q = attr(&p, f->pos, 0);
        for (int i = 0; i < 3; i++) v->pos[i] = i < n ? read_comp(q + i * cs, f->pos_fmt, f->pos_shift) : 0.f;
        if (f->pos == 1) p += n * cs;
    }
    // normal (skipped for now)
    if (f->nrm) {
        int cs = comp_size(f->nrm_fmt);
        if (f->nrm == 1) p += cs * 3 * (f->nrm_cnt ? 3 : 1);
        else p += idx_size(f->nrm) * ((f->nrm_cnt && (cp[0x70] >> 31)) ? 3 : 1);
    }
    // colors
    v->has_col = 0;
    for (int c = 0; c < 2; c++) {
        if (!f->col[c]) continue;
        const uint8_t* q = attr(&p, f->col[c], 2 + c);
        float col[4];
        read_color(q, f->col_fmt[c], col);
        if (f->col[c] == 1) p += col_size(f->col_fmt[c]);
        if (c == 0) { memcpy(v->col, col, sizeof(col)); v->has_col = 1; }
    }
    // texture coordinates
    v->has_tex = 0;
    for (int t = 0; t < 8; t++) {
        if (!f->tex[t]) continue;
        int n = f->tex_cnt[t] ? 2 : 1, cs = comp_size(f->tex_fmt[t]);
        const uint8_t* q = attr(&p, f->tex[t], 4 + t);
        if (t == 0) {
            v->tex[0] = read_comp(q, f->tex_fmt[t], f->tex_shift[t]);
            v->tex[1] = n > 1 ? read_comp(q + cs, f->tex_fmt[t], f->tex_shift[t]) : 0.f;
            v->has_tex = 1;
        }
        if (f->tex[t] == 1) p += n * cs;
    }
    return p;
}

// --------------------------------------------------------- transforms
static void transform(const Vtx* v, float out[4]) {
    uint32_t m = (uint32_t)v->pm * 4u;
    float e[3];
    for (int r = 0; r < 3; r++)
        e[r] = xf_f(m + r * 4) * v->pos[0] + xf_f(m + r * 4 + 1) * v->pos[1] + xf_f(m + r * 4 + 2) * v->pos[2] +
               xf_f(m + r * 4 + 3);
    float p0 = xf_f(0x1020), p1 = xf_f(0x1021), p2 = xf_f(0x1022), p3 = xf_f(0x1023), p4 = xf_f(0x1024),
          p5 = xf_f(0x1025);
    if (xf_mem[0x1026] == 0) {  // perspective
        out[0] = p0 * e[0] + p1 * e[2];
        out[1] = p2 * e[1] + p3 * e[2];
        out[2] = p4 * e[2] + p5;
        out[3] = -e[2];
    } else {                    // orthographic
        out[0] = p0 * e[0] + p1;
        out[1] = p2 * e[1] + p3;
        out[2] = p4 * e[2] + p5;
        out[3] = 1.0f;
    }
    out[2] = 2.0f * out[2] + out[3];  // GX clip z in [-w, 0] -> GL [-w, w]
}

// ------------------------------------------------------------- render
static void apply_viewport(void) {
    int ww, wh;
    gx_win_size(&ww, &wh);
    float sx = xf_f(0x101A), sy = xf_f(0x101B), ox = xf_f(0x101D), oy = xf_f(0x101E);
    float left = ox - 342.0f - sx, top = oy - 342.0f + sy;
    float w = 2.0f * sx, h = -2.0f * sy;
    if (w <= 0 || h <= 0) { left = 0; top = 0; w = 640; h = 480; }
    float kx = (float)ww / 640.0f, ky = (float)wh / 480.0f;
    glViewport((GLint)(left * kx), (GLint)((480.0f - (top + h)) * ky), (GLsizei)(w * kx), (GLsizei)(h * ky));
}

static void apply_blend_depth(void) {
    uint32_t cm = bp[0x41];
    if (cm & 1) {
        static const GLenum fac[8] = {GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
                                      GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
        static const GLenum dfac[8] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR,
                                       GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
        glEnable(GL_BLEND);
        glBlendFunc(fac[(cm >> 8) & 7], dfac[(cm >> 5) & 7]);
    } else {
        glDisable(GL_BLEND);
    }
    uint32_t zm = bp[0x40];
    if (zm & 1) {
        static const GLenum fn[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(fn[(zm >> 1) & 7]);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask((zm >> 4) & 1 ? GL_TRUE : GL_FALSE);
    glColorMask((cm >> 3) & 1, (cm >> 3) & 1, (cm >> 3) & 1, (cm >> 4) & 1);
}

static int bind_texture0(void) {
    uint32_t tref = bp[0x28];
    int map = (int)(tref & 7);
    if (!((tref >> 6) & 1)) { glDisable(GL_TEXTURE_2D); return 0; }
    static const uint8_t img0[8] = {0x88, 0x89, 0x8A, 0x8B, 0xA8, 0xA9, 0xAA, 0xAB};
    static const uint8_t img3[8] = {0x94, 0x95, 0x96, 0x97, 0xB4, 0xB5, 0xB6, 0xB7};
    static const uint8_t tlut[8] = {0x98, 0x99, 0x9A, 0x9B, 0xB8, 0xB9, 0xBA, 0xBB};
    static const uint8_t mode0[8] = {0x80, 0x81, 0x82, 0x83, 0xA0, 0xA1, 0xA2, 0xA3};
    uint32_t i0 = bp[img0[map]], i3 = bp[img3[map]], tl = bp[tlut[map]], m0 = bp[mode0[map]];
    int w = (int)(i0 & 0x3FF) + 1, h = (int)((i0 >> 10) & 0x3FF) + 1, fmt = (int)((i0 >> 20) & 15);
    uint32_t addr = 0x80000000u | ((i3 & 0x00FFFFFFu) << 5);
    unsigned id = gx_tex_get(addr, w, h, fmt, (tl & 0x3FF) << 9, (int)((tl >> 10) & 3), tmem);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, id);
    static const GLint wrap[4] = {0x812F /* CLAMP_TO_EDGE */, GL_REPEAT, GL_REPEAT, GL_REPEAT};
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap[m0 & 3]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap[(m0 >> 2) & 3]);
    return 1;
}

static void mat_color(float* o) {
    uint32_t c = xf_mem[0x100C];
    o[0] = (c >> 24) / 255.f; o[1] = ((c >> 16) & 255) / 255.f; o[2] = ((c >> 8) & 255) / 255.f; o[3] = (c & 255) / 255.f;
}

static void draw(int prim, int vat, int count, const uint8_t* data) {
    VtxFmt f;
    get_fmt(vat, &f);
    static const GLenum mode[8] = {GL_QUADS, GL_QUADS, GL_TRIANGLES, GL_TRIANGLE_STRIP,
                                   GL_TRIANGLE_FAN, GL_LINES, GL_LINE_STRIP, GL_POINTS};
    apply_viewport();
    apply_blend_depth();
    int tex = bind_texture0();
    uint32_t chan = xf_mem[0x100E];  // color0 control: bit0 = material from vertex
    float mat[4];
    mat_color(mat);
    glBegin(mode[prim]);
    for (int i = 0; i < count; i++) {
        Vtx v;
        data = decode_vertex(data, &f, &v);
        float c[4];
        if ((chan & 1) && v.has_col) memcpy(c, v.col, sizeof(c));
        else memcpy(c, mat, sizeof(c));
        glColor4fv(c);
        if (tex && v.has_tex) glTexCoord2f(v.tex[0], v.tex[1]);
        float o[4];
        transform(&v, o);
        glVertex4fv(o);
    }
    glEnd();
    st.draws++;
    st.verts += (unsigned long long)count;
}

// ---------------------------------------------------- BP side effects
static void pe_raise(int finish, uint16_t token);

static void bp_write(uint32_t v) {
    uint32_t reg = v >> 24, val = v & 0xFFFFFF;
    // BP mask (0xFE) applies to the next write only
    if (reg == 0xFE) { bp[0xFE] = val; return; }
    uint32_t mask = bp[0xFE] ? bp[0xFE] : 0xFFFFFF;
    bp[0xFE] = 0;
    bp[reg] = (bp[reg] & ~mask) | (val & mask);
    switch (reg) {
    case 0x45: pe_raise(1, 0); break;                        // draw done
    case 0x47: pe_raise(0, (uint16_t)(val & 0xFFFF)); break;  // token (no irq)
    case 0x48: pe_raise(2, (uint16_t)(val & 0xFFFF)); break;  // token + irq
    case 0x52: {                                              // EFB copy
        int to_xfb = (val >> 14) & 1, clear = (val >> 11) & 1;
        if (to_xfb) {
            st.frames++;
            static int shot;
            char* dir = getenv("GX_SCREENSHOT_DIR");
            if (dir && (st.frames % 60 == 0) && shot < 20) {
                char path[512];
                snprintf(path, sizeof(path), "%s/frame_%05llu.bmp", dir, st.frames);
                gx_win_screenshot(path);
                shot++;
            }
            gx_win_present();
            gx_tex_invalidate();
        }
        if (clear) {
            float r = (bp[0x4F] & 255) / 255.f, a = ((bp[0x4F] >> 8) & 255) / 255.f;
            float b = (bp[0x50] & 255) / 255.f, g = ((bp[0x50] >> 8) & 255) / 255.f;
            glViewport(0, 0, 0x7FFF, 0x7FFF);
            int ww, wh;
            gx_win_size(&ww, &wh);
            glViewport(0, 0, ww, wh);
            glClearColor(r, g, b, a);
            glClearDepth(1.0);
            glColorMask(1, 1, 1, 1);
            glDepthMask(GL_TRUE);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        }
        break;
    }
    case 0x65: {  // load TLUT: RAM (BP 0x64) -> TMEM
        uint32_t src = 0x80000000u | ((bp[0x64] & 0x1FFFFF) << 5);
        uint32_t dst = (val & 0x3FF) << 9, cnt = ((val >> 10) & 0x7FF) * 32;
        if (dst + cnt <= 0x100000) memcpy(tmem + dst, ppc_ptr(src), cnt);
        break;
    }
    default: break;
    }
}

// ------------------------------------------------- PE (draw sync) regs
#define PE_CTRL 0x100A
#define PE_TOKEN 0x100E
static void pe_raise(int kind, uint16_t token) {
    uint16_t ctrl = ppc_mmio_get16(PE_CTRL);
    if (kind == 1) {
        ppc_mmio_set16(PE_CTRL, (uint16_t)(ctrl | 0x8));
        if (ctrl & 0x2) hle_irq_raise(IRQ_PI_PE_FINISH);
    } else {
        ppc_mmio_set16(PE_TOKEN, token);
        if (kind == 2) {
            ppc_mmio_set16(PE_CTRL, (uint16_t)(ctrl | 0x4));
            if (ctrl & 0x1) hle_irq_raise(IRQ_PI_PE_TOKEN);
        }
    }
}

// ------------------------------------------------------ command parser
static void xf_load(uint32_t addr, uint32_t n, const uint8_t* p) {
    for (uint32_t i = 0; i < n; i++)
        if (addr + i < 0x1100) xf_mem[addr + i] = rd32(p + 4 * i);
}

static void run_list(const uint8_t* p, size_t len);

// Parses one command at p (len bytes available). Returns bytes consumed,
// or 0 if more data is needed.
static size_t parse(const uint8_t* p, size_t len) {
    uint8_t op = p[0];
    switch (op) {
    case 0x00: return 1;
    case 0x08:
        if (len < 6) return 0;
        cp[rd8(p + 1)] = rd32(p + 2);
        return 6;
    case 0x10: {
        if (len < 5) return 0;
        uint32_t n = rd16(p + 1) + 1, addr = rd16(p + 3);
        if (len < 5 + 4 * n) return 0;
        xf_load(addr, n, p + 5);
        return 5 + 4 * n;
    }
    case 0x20: case 0x28: case 0x30: case 0x38: {
        if (len < 5) return 0;
        uint32_t v = rd32(p + 1), idx = v >> 16, n = ((v >> 12) & 15) + 1, addr = v & 0xFFF;
        int arr = 12 + ((op - 0x20) >> 3);
        uint32_t src = 0x80000000u | ((cp[0xA0 + arr] + idx * (cp[0xB0 + arr] & 0xFF)) & 0x01FFFFFFu);
        xf_load(addr, n, ppc_ptr(src));
        return 5;
    }
    case 0x40: {
        if (len < 9) return 0;
        uint32_t addr = rd32(p + 1), size = rd32(p + 5);
        run_list(ppc_ptr(0x80000000u | (addr & 0x01FFFFFFu)), size);
        return 9;
    }
    case 0x44: case 0x48: return 1;
    case 0x61:
        if (len < 5) return 0;
        bp_write(rd32(p + 1));
        return 5;
    default:
        if (op >= 0x80) {
            if (len < 3) return 0;
            VtxFmt f;
            int vat = op & 7, prim = (op >> 3) & 7, count = (int)rd16(p + 1);
            get_fmt(vat, &f);
            size_t need = 3 + (size_t)count * (size_t)vtx_size(&f);
            if (len < need) return 0;
            draw(prim, vat, count, p + 3);
            return need;
        }
        st.unknown++;
        return 1;
    }
}

static void run_list(const uint8_t* p, size_t len) {
    size_t i = 0;
    while (i < len) {
        size_t n = parse(p + i, len - i);
        if (!n) break;
        i += n;
        st.cmds++;
    }
}

static void fifo_push(uint32_t v, int size) {
    if (fifo_len + 4 > sizeof(fifo)) fifo_len = 0;  // should not happen: drop
    for (int i = size - 1; i >= 0; i--) fifo[fifo_len++] = (uint8_t)(v >> (8 * i));
    size_t i = 0;
    for (;;) {
        size_t n = parse(fifo + i, fifo_len - i);
        if (!n) break;
        i += n;
        st.cmds++;
        if (i >= fifo_len) break;
    }
    if (i) {
        memmove(fifo, fifo + i, fifo_len - i);
        fifo_len -= i;
    }
}

// -------------------------------------------------- MMIO hook chaining
static int (*next_write)(uint32_t, uint32_t, int);
static int (*next_read)(uint32_t, int, uint32_t*);

static int write_hook(uint32_t off, uint32_t v, int size) {
    if ((off & 0xF000) == 0x8000) {
        if (!gx_ready) { gx_win_open(); gx_ready = 1; }
        fifo_push(v, size);
        return 1;
    }
    if (off == PE_CTRL && size == 2) {  // status bits 2/3 are write-one-to-clear
        uint16_t old = ppc_mmio_get16(PE_CTRL);
        uint16_t nv = (uint16_t)((v & 0x3) | (old & 0xC & ~v));
        ppc_mmio_set16(PE_CTRL, nv);
        return 1;
    }
    return next_write ? next_write(off, v, size) : 0;
}

static int read_hook(uint32_t off, int size, uint32_t* v) {
    if (off == 0x0000 && size == 2) { *v = 0x000C; return 1; }  // CP status: GP idle
    return next_read ? next_read(off, size, v) : 0;
}

void gx_install(void) {
    tmem = (uint8_t*)calloc(1, 0x100000);
    next_write = ppc_mmio_write_hook;
    next_read = ppc_mmio_read_hook;
    ppc_mmio_write_hook = write_hook;
    ppc_mmio_read_hook = read_hook;
}

void gx_stats(void) {
    fprintf(stderr, "gx: %llu commands, %llu draws, %llu vertices, %llu frames, %llu unknown bytes\n",
            st.cmds, st.draws, st.verts, st.frames, st.unknown);
}
