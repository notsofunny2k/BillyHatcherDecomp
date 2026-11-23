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

// public (gx.c)
void gx_install(void);
void gx_stats(void);

#endif
