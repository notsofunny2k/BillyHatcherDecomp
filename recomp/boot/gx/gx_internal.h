#ifndef GX_INTERNAL_H
#define GX_INTERNAL_H

#include <stdint.h>

// window (gx_win.c)
int gx_win_open(void);
void gx_win_pump(void);
void gx_win_present(void);
void gx_win_size(int* w, int* h);
void gx_win_screenshot(const char* path);

// textures (gx_tex.c): decodes a GX texture to RGBA8, returns a GL texture id
unsigned gx_tex_get(uint32_t addr, int w, int h, int fmt, uint32_t tlut_tmem, int tlut_fmt,
                    const uint8_t* tmem);
void gx_tex_invalidate(void);
void gx_tex_efb_copy(uint32_t addr, int x, int y, int w, int h);  // window pixels
unsigned gx_tex_efb_lookup(uint32_t addr);

// shaders / modern GL entry points (gx_shader.c)
int gx_gl_load(void);
void gx_gl_active_texture(int unit);
void gx_gl_blend_equation(unsigned e);
void gx_gl_attrib(int idx, int size, const float* data);
int gx_shader_bind(const uint32_t* bp, const float k[16], const float reg[16], float aref0, float aref1,
                   const float fog[4], const float fogcol[4], const float vpz[4], const float flip[8]);
int gx_shader_count(void);

// boot_main.c: prints DVD/GX/audio/thread statistics
void boot_print_stats(void);

// public (gx.c)
void gx_install(void);
void gx_stats(void);

#endif
