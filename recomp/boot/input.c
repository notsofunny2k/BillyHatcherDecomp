// Host input -> GameCube controller state.
//
// Port 1: keyboard (when the game window has focus) merged with XInput pad 1.
// Ports 2-4: XInput pads 2-4.
//
// Keyboard (Dolphin-like defaults):
//   Arrows = control stick     Enter = START
//   X = A   Z = B   C = X   S = Y   D = Z   Q = L   W = R
//   I/J/K/L = C-stick          T/G/F/H = D-pad up/down/left/right
//   Shift held = half-tilt (walk)
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "input.h"

// GameCube button bits (PADStatus.button)
enum {
    GC_LEFT = 0x0001, GC_RIGHT = 0x0002, GC_DOWN = 0x0004, GC_UP = 0x0008,
    GC_Z = 0x0010, GC_R = 0x0020, GC_L = 0x0040,
    GC_A = 0x0100, GC_B = 0x0200, GC_X = 0x0400, GC_Y = 0x0800, GC_START = 0x1000,
};

// ---- XInput, loaded at runtime ------------------------------------------
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XPad;
typedef struct { DWORD dwPacketNumber; XPad Gamepad; } XState;
typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, XState*);
static XInputGetStateFn xinput_get;
static int xinput_tried;

static void load_xinput(void) {
    if (xinput_tried) return;
    xinput_tried = 1;
    const char* dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (int i = 0; i < 3 && !xinput_get; i++) {
        HMODULE m = LoadLibraryA(dlls[i]);
        if (m) xinput_get = (XInputGetStateFn)(void*)GetProcAddress(m, "XInputGetState");
    }
}

enum { XI_UP = 0x1, XI_DOWN = 0x2, XI_LEFT = 0x4, XI_RIGHT = 0x8, XI_START = 0x10, XI_BACK = 0x20,
       XI_LB = 0x100, XI_RB = 0x200, XI_A = 0x1000, XI_B = 0x2000, XI_X = 0x4000, XI_Y = 0x8000 };

// Stick axis (-32768..32767, with deadzone) -> GC offset (about -100..100)
static int axis(int v, int dead) {
    if (v > -dead && v < dead) return 0;
    double f = (double)(v - (v > 0 ? dead : -dead)) / (32767.0 - dead);
    if (f > 1) f = 1;
    if (f < -1) f = -1;
    return (int)lround(f * 100.0);
}

typedef struct { int buttons, sx, sy, cx, cy, tl, tr, present; } Pad;

static int read_xinput(int port, Pad* p) {
    load_xinput();
    if (!xinput_get) return 0;
    XState s;
    if (xinput_get((DWORD)port, &s) != 0) return 0;
    XPad* g = &s.Gamepad;
    if (g->wButtons & XI_A) p->buttons |= GC_A;
    if (g->wButtons & XI_B) p->buttons |= GC_B;
    if (g->wButtons & XI_X) p->buttons |= GC_X;
    if (g->wButtons & XI_Y) p->buttons |= GC_Y;
    if (g->wButtons & XI_START) p->buttons |= GC_START;
    if (g->wButtons & XI_RB) p->buttons |= GC_Z;
    if (g->wButtons & XI_UP) p->buttons |= GC_UP;
    if (g->wButtons & XI_DOWN) p->buttons |= GC_DOWN;
    if (g->wButtons & XI_LEFT) p->buttons |= GC_LEFT;
    if (g->wButtons & XI_RIGHT) p->buttons |= GC_RIGHT;
    int sx = axis(g->sThumbLX, 7849), sy = axis(g->sThumbLY, 7849);
    int cx = axis(g->sThumbRX, 8689), cy = axis(g->sThumbRY, 8689);
    if (sx || sy) { p->sx = sx; p->sy = sy; }
    if (cx || cy) { p->cx = cx; p->cy = cy; }
    if (g->bLeftTrigger > p->tl) p->tl = g->bLeftTrigger;
    if (g->bRightTrigger > p->tr) p->tr = g->bRightTrigger;
    if (g->bLeftTrigger > 230) p->buttons |= GC_L;
    if (g->bRightTrigger > 230) p->buttons |= GC_R;
    return 1;
}

static int key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void read_keyboard(Pad* p) {
    if (!input_window_focused()) return;
    int full = key(VK_SHIFT) ? 50 : 100;
    if (key(VK_LEFT)) p->sx = -full;
    if (key(VK_RIGHT)) p->sx = full;
    if (key(VK_UP)) p->sy = full;
    if (key(VK_DOWN)) p->sy = -full;
    if (key('J')) p->cx = -100;
    if (key('L')) p->cx = 100;
    if (key('I')) p->cy = 100;
    if (key('K')) p->cy = -100;
    if (key('X')) p->buttons |= GC_A;
    if (key('Z')) p->buttons |= GC_B;
    if (key('C')) p->buttons |= GC_X;
    if (key('S')) p->buttons |= GC_Y;
    if (key('D')) p->buttons |= GC_Z;
    if (key(VK_RETURN)) p->buttons |= GC_START;
    if (key('Q')) { p->buttons |= GC_L; p->tl = 255; }
    if (key('W')) { p->buttons |= GC_R; p->tr = 255; }
    if (key('T')) p->buttons |= GC_UP;
    if (key('G')) p->buttons |= GC_DOWN;
    if (key('F')) p->buttons |= GC_LEFT;
    if (key('H')) p->buttons |= GC_RIGHT;
}

// Debug/testing: INPUT_AUTOPRESS="START:12,A:16.5,B:20" presses a button on
// port 1 for 150 ms at the given number of seconds after start.
static void read_autopress(Pad* p) {
    static const char* spec;
    static int init;
    static ULONGLONG t0;
    if (!init) { init = 1; spec = getenv("INPUT_AUTOPRESS"); t0 = GetTickCount64(); }
    if (!spec) return;
    double now = (double)(GetTickCount64() - t0) / 1000.0;
    static const struct { const char* name; int bit; } names[] = {
        {"START", GC_START}, {"A", GC_A}, {"B", GC_B}, {"X", GC_X}, {"Y", GC_Y}, {"Z", GC_Z},
        {"L", GC_L}, {"R", GC_R}, {"UP", GC_UP}, {"DOWN", GC_DOWN}, {"LEFT", GC_LEFT}, {"RIGHT", GC_RIGHT}};
    char buf[512];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (char* tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        char* colon = strchr(tok, ':');
        if (!colon) continue;
        *colon = 0;
        double at = atof(colon + 1);
        if (now >= at && now < at + 0.15)
            for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
                if (!_stricmp(tok, names[i].name)) p->buttons |= names[i].bit;
    }
}

static unsigned char clamp_u8(int v) { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }

int input_connected(int chan) {
    if (chan == 0) return 1;
    Pad p;
    memset(&p, 0, sizeof(p));
    return read_xinput(chan, &p);
}

int input_read(int chan, unsigned char out[8]) {
    Pad p;
    memset(&p, 0, sizeof(p));
    int ok = read_xinput(chan, &p);
    if (chan == 0) { read_keyboard(&p); read_autopress(&p); ok = 1; }
    if (!ok) return 0;
    // SI response for "read status" (analog mode 3), decoded by PADRead:
    // bytes 0-1 buttons, 2-3 stick, 4-5 C-stick, 6-7 analog L/R; sticks centered at 0x80
    int b = p.buttons | 0x0080;  // bit 7: "use origin"
    out[0] = (unsigned char)(b >> 8);
    out[1] = (unsigned char)b;
    out[2] = clamp_u8(128 + p.sx);
    out[3] = clamp_u8(128 + p.sy);
    out[4] = clamp_u8(128 + p.cx);
    out[5] = clamp_u8(128 + p.cy);
    out[6] = clamp_u8(p.tl);
    out[7] = clamp_u8(p.tr);
    return 1;
}
