// TEV -> GLSL: generates a fragment shader for the current TEV configuration
// (stages, inputs, konst colors, swap tables, alpha test) and caches programs.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <GL/gl.h>

#include "gx_internal.h"

// ---- GL 2.0 entry points ------------------------------------------------
typedef char GLchar;
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_TEXTURE0 0x84C0
#define FN(ret, name, args) typedef ret(APIENTRY* PFN_##name) args; static PFN_##name p_##name;
FN(GLuint, glCreateShader, (GLenum))
FN(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))
FN(void, glCompileShader, (GLuint))
FN(void, glGetShaderiv, (GLuint, GLenum, GLint*))
FN(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))
FN(GLuint, glCreateProgram, (void))
FN(void, glAttachShader, (GLuint, GLuint))
FN(void, glBindAttribLocation, (GLuint, GLuint, const GLchar*))
FN(void, glLinkProgram, (GLuint))
FN(void, glGetProgramiv, (GLuint, GLenum, GLint*))
FN(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))
FN(void, glUseProgram, (GLuint))
FN(GLint, glGetUniformLocation, (GLuint, const GLchar*))
FN(void, glUniform1i, (GLint, GLint))
FN(void, glUniform4fv, (GLint, GLsizei, const GLfloat*))
FN(void, glActiveTexture, (GLenum))
FN(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))
FN(void, glEnableVertexAttribArray, (GLuint))
FN(void, glDisableVertexAttribArray, (GLuint))
FN(void, glBlendEquation, (GLenum))

static int loaded;
#define LOAD(name)                                                    \
    p_##name = (PFN_##name)(void*)wglGetProcAddress(#name);           \
    if (!p_##name) { fprintf(stderr, "gx: missing %s\n", #name); ok = 0; }

int gx_gl_load(void) {
    if (loaded) return loaded > 0;
    int ok = 1;
    LOAD(glCreateShader) LOAD(glShaderSource) LOAD(glCompileShader) LOAD(glGetShaderiv)
    LOAD(glGetShaderInfoLog) LOAD(glCreateProgram) LOAD(glAttachShader) LOAD(glBindAttribLocation)
    LOAD(glLinkProgram) LOAD(glGetProgramiv) LOAD(glGetProgramInfoLog) LOAD(glUseProgram)
    LOAD(glGetUniformLocation) LOAD(glUniform1i) LOAD(glUniform4fv) LOAD(glActiveTexture)
    LOAD(glVertexAttribPointer) LOAD(glEnableVertexAttribArray) LOAD(glDisableVertexAttribArray)
    LOAD(glBlendEquation)
    loaded = ok ? 1 : -1;
    return ok;
}

void gx_gl_active_texture(int unit) { p_glActiveTexture(GL_TEXTURE0 + (GLenum)unit); }
void gx_gl_blend_equation(unsigned e) { p_glBlendEquation(e); }
void gx_gl_attrib(int idx, int size, const float* data) {
    if (data) {
        p_glEnableVertexAttribArray((GLuint)idx);
        p_glVertexAttribPointer((GLuint)idx, size, GL_FLOAT, GL_FALSE, 0, data);
    } else {
        p_glDisableVertexAttribArray((GLuint)idx);
    }
}

// ---- shader generation ----------------------------------------------------
static const char* VS =
    "#version 120\n"
    "attribute vec4 a_pos; attribute vec4 a_col0; attribute vec4 a_col1;\n"
    "attribute vec3 a_tex0, a_tex1, a_tex2, a_tex3, a_tex4, a_tex5, a_tex6, a_tex7;\n"
    "varying vec4 v_col0, v_col1; varying vec3 v_tex[8];\n"
    "void main() {\n"
    "  gl_Position = a_pos; v_col0 = a_col0; v_col1 = a_col1;\n"
    "  v_tex[0]=a_tex0; v_tex[1]=a_tex1; v_tex[2]=a_tex2; v_tex[3]=a_tex3;\n"
    "  v_tex[4]=a_tex4; v_tex[5]=a_tex5; v_tex[6]=a_tex6; v_tex[7]=a_tex7;\n"
    "}\n";

static const char* swz = "rgba";

static void swap_str(const uint32_t* bp, int table, char out[5]) {
    uint32_t a = bp[0xF6 + table * 2], b = bp[0xF7 + table * 2];
    out[0] = swz[a & 3]; out[1] = swz[(a >> 2) & 3];
    out[2] = swz[b & 3]; out[3] = swz[(b >> 2) & 3];
    out[4] = 0;
}

static const char* kc_expr(int kc, char* buf) {
    static const char* consts[8] = {"1.0", "0.875", "0.75", "0.625", "0.5", "0.375", "0.25", "0.125"};
    if (kc < 8) { sprintf(buf, "vec3(%s)", consts[kc]); return buf; }
    if (kc >= 12 && kc < 16) { sprintf(buf, "u_k[%d].rgb", kc - 12); return buf; }
    if (kc >= 16) { sprintf(buf, "vec3(u_k[%d].%c)", (kc - 16) & 3, "rgba"[(kc - 16) >> 2]); return buf; }
    return "vec3(1.0)";
}

static const char* ka_expr(int ka, char* buf) {
    static const char* consts[8] = {"1.0", "0.875", "0.75", "0.625", "0.5", "0.375", "0.25", "0.125"};
    if (ka < 8) return consts[ka];
    if (ka >= 16) { sprintf(buf, "u_k[%d].%c", (ka - 16) & 3, "rgba"[(ka - 16) >> 2]); return buf; }
    return "1.0";
}

static const char* cin(int i) {
    static const char* t[16] = {"prev.rgb", "prev.aaa", "c0.rgb", "c0.aaa", "c1.rgb", "c1.aaa", "c2.rgb", "c2.aaa",
                                "tex.rgb", "tex.aaa", "ras.rgb", "ras.aaa", "vec3(1.0)", "vec3(0.5)", "kc", "vec3(0.0)"};
    return t[i & 15];
}
static const char* ain(int i) {
    static const char* t[8] = {"prev.a", "c0.a", "c1.a", "c2.a", "tex.a", "ras.a", "ka", "0.0"};
    return t[i & 7];
}
static const char* dreg(int d) {
    static const char* t[4] = {"prev", "c0", "c1", "c2"};
    return t[d & 3];
}

static void gen_fs(const uint32_t* bp, char* s, size_t cap) {
    size_t n = 0;
#define P(...) n += (size_t)snprintf(s + n, cap - n, __VA_ARGS__)
    int stages = (int)((bp[0x00] >> 10) & 15) + 1;
    P("#version 120\n");
    P("varying vec4 v_col0, v_col1; varying vec3 v_tex[8];\n");
    P("uniform sampler2D s_tex[8]; uniform vec4 u_k[4]; uniform vec4 u_reg[4]; uniform vec4 u_aref;\n");
    P("void main() {\n");
    P("  vec4 prev = u_reg[0], c0 = u_reg[1], c1 = u_reg[2], c2 = u_reg[3];\n");
    P("  vec4 tex, ras; vec3 kc; float ka;\n");
    for (int i = 0; i < stages; i++) {
        uint32_t tref = (bp[0x28 + i / 2] >> ((i & 1) * 12)) & 0xFFF;
        int map = tref & 7, coord = (tref >> 3) & 7, en = (tref >> 6) & 1, chan = (tref >> 7) & 7;
        uint32_t ce = bp[0xC0 + 2 * i], ae = bp[0xC1 + 2 * i];
        uint32_t ksel = bp[0xF6 + i / 2];
        int kc = (int)((ksel >> (4 + (i & 1) * 10)) & 31), ka = (int)((ksel >> (9 + (i & 1) * 10)) & 31);
        char rs[5], ts[5], kb[32], kab[32];
        swap_str(bp, (int)(ae & 3), rs);
        swap_str(bp, (int)((ae >> 2) & 3), ts);
        P("  // stage %d\n", i);
        if (en) P("  tex = texture2DProj(s_tex[%d], v_tex[%d]).%s;\n", map, coord, ts);
        else P("  tex = vec4(1.0);\n");
        if (chan == 0) P("  ras = v_col0.%s;\n", rs);
        else if (chan == 1) P("  ras = v_col1.%s;\n", rs);
        else P("  ras = vec4(0.0);\n");
        P("  kc = %s; ka = %s;\n", kc_expr(kc, kb), ka_expr(ka, kab));
        // color combiner
        {
            int d = ce & 15, c = (ce >> 4) & 15, b = (ce >> 8) & 15, a = (ce >> 12) & 15;
            int bias = (ce >> 16) & 3, sub = (ce >> 18) & 1, clampv = (ce >> 19) & 1, scale = (ce >> 20) & 3,
                dst = (ce >> 22) & 3;
            const char* bs[4] = {"0.0", "0.5", "-0.5", "0.0"};
            const char* sc[4] = {"1.0", "2.0", "4.0", "0.5"};
            P("  { vec3 A = clamp(%s,0.0,1.0), B = clamp(%s,0.0,1.0), C = clamp(%s,0.0,1.0);\n", cin(a), cin(b), cin(c));
            P("    vec3 r = (%s %c mix(A, B, C) + vec3(%s)) * %s;\n", cin(d), sub ? '-' : '+', bs[bias], sc[scale]);
            P("    %s.rgb = %s; }\n", dreg(dst),
              clampv ? "clamp(r, 0.0, 1.0)" : "clamp(r, -4.0, 4.0)");
        }
        // alpha combiner
        {
            int d = (ae >> 4) & 7, c = (ae >> 7) & 7, b = (ae >> 10) & 7, a = (ae >> 13) & 7;
            int bias = (ae >> 16) & 3, sub = (ae >> 18) & 1, clampv = (ae >> 19) & 1, scale = (ae >> 20) & 3,
                dst = (ae >> 22) & 3;
            const char* bs[4] = {"0.0", "0.5", "-0.5", "0.0"};
            const char* sc[4] = {"1.0", "2.0", "4.0", "0.5"};
            P("  { float A = clamp(%s,0.0,1.0), B = clamp(%s,0.0,1.0), C = clamp(%s,0.0,1.0);\n", ain(a), ain(b), ain(c));
            P("    float r = (%s %c mix(A, B, C) + %s) * %s;\n", ain(d), sub ? '-' : '+', bs[bias], sc[scale]);
            P("    %s.a = %s; }\n", dreg(dst), clampv ? "clamp(r, 0.0, 1.0)" : "clamp(r, -4.0, 4.0)");
        }
    }
    // alpha test
    {
        uint32_t ac = bp[0xF3];
        int c0f = (ac >> 16) & 7, c1f = (ac >> 19) & 7, logic = (ac >> 22) & 3;
        const char* ops[8] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
        char e0[64], e1[64];
        if (c0f == 0 || c0f == 7) snprintf(e0, sizeof(e0), "%s", c0f ? "true" : "false");
        else snprintf(e0, sizeof(e0), "(prev.a %s u_aref.x)", ops[c0f]);
        if (c1f == 0 || c1f == 7) snprintf(e1, sizeof(e1), "%s", c1f ? "true" : "false");
        else snprintf(e1, sizeof(e1), "(prev.a %s u_aref.y)", ops[c1f]);
        const char* lg[4] = {"&&", "||", "!=", "=="};
        if (!(c0f == 7 && c1f == 7 && logic == 0))
            P("  if (!(%s %s %s)) discard;\n", e0, lg[logic], e1);
    }
    P("  gl_FragColor = clamp(prev, 0.0, 1.0);\n}\n");
#undef P
}

// key: the BP registers that shape the shader
static int make_key(const uint32_t* bp, uint32_t* key) {
    int stages = (int)((bp[0x00] >> 10) & 15) + 1, n = 0;
    key[n++] = (uint32_t)stages;
    for (int i = 0; i < 8; i++) key[n++] = bp[0x28 + i];
    for (int i = 0; i < stages; i++) { key[n++] = bp[0xC0 + 2 * i]; key[n++] = bp[0xC1 + 2 * i]; }
    for (int i = 0; i < 8; i++) key[n++] = bp[0xF6 + i];
    key[n++] = bp[0xF3] & 0xFF0000u;  // compare functions + logic (refs are uniforms)
    return n;
}

typedef struct {
    uint32_t key[64];
    int klen;
    GLuint prog;
    GLint u_k, u_reg, u_aref;
} Prog;

#define NPROG 1024
static Prog progs[NPROG];
static int nprogs;
static unsigned long long compiles;

static GLuint compile(GLenum type, const char* src) {
    GLuint sh = p_glCreateShader(type);
    p_glShaderSource(sh, 1, &src, NULL);
    p_glCompileShader(sh);
    GLint ok = 0;
    p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        p_glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "gx: shader compile failed:\n%s\n%s\n", log, src);
    }
    return sh;
}

static GLuint vs_obj;

// Activates the program for the current TEV state and loads its uniforms.
int gx_shader_bind(const uint32_t* bp, const float k[16], const float reg[16], float aref0, float aref1) {
    uint32_t key[64];
    int klen = make_key(bp, key);
    uint32_t h = 2166136261u;
    for (int i = 0; i < klen; i++) h = (h ^ key[i]) * 16777619u;
    int slot = (int)(h % NPROG);
    Prog* p = NULL;
    for (int i = 0; i < NPROG; i++) {
        Prog* q = &progs[(slot + i) % NPROG];
        if (!q->prog) { p = q; break; }
        if (q->klen == klen && !memcmp(q->key, key, (size_t)klen * 4)) { p = q; break; }
    }
    if (!p) return 0;
    if (!p->prog) {
        if (!vs_obj) vs_obj = compile(GL_VERTEX_SHADER, VS);
        static char fs[32768];
        gen_fs(bp, fs, sizeof(fs));
        GLuint f = compile(GL_FRAGMENT_SHADER, fs);
        GLuint prog = p_glCreateProgram();
        p_glAttachShader(prog, vs_obj);
        p_glAttachShader(prog, f);
        static const char* attrs[11] = {"a_pos", "a_col0", "a_col1", "a_tex0", "a_tex1", "a_tex2",
                                        "a_tex3", "a_tex4", "a_tex5", "a_tex6", "a_tex7"};
        for (int i = 0; i < 11; i++) p_glBindAttribLocation(prog, (GLuint)i, attrs[i]);
        p_glLinkProgram(prog);
        GLint ok = 0;
        p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[2048];
            p_glGetProgramInfoLog(prog, sizeof(log), NULL, log);
            fprintf(stderr, "gx: program link failed: %s\n", log);
        }
        p_glUseProgram(prog);
        char name[32];
        for (int i = 0; i < 8; i++) {
            snprintf(name, sizeof(name), "s_tex[%d]", i);
            p_glUniform1i(p_glGetUniformLocation(prog, name), i);
        }
        memcpy(p->key, key, (size_t)klen * 4);
        p->klen = klen;
        p->prog = prog;
        p->u_k = p_glGetUniformLocation(prog, "u_k");
        p->u_reg = p_glGetUniformLocation(prog, "u_reg");
        p->u_aref = p_glGetUniformLocation(prog, "u_aref");
        nprogs++;
        compiles++;
    }
    p_glUseProgram(p->prog);
    p_glUniform4fv(p->u_k, 4, k);
    p_glUniform4fv(p->u_reg, 4, reg);
    float ar[4] = {aref0, aref1, 0, 0};
    p_glUniform4fv(p->u_aref, 1, ar);
    return 1;
}

int gx_shader_count(void) { return nprogs; }
