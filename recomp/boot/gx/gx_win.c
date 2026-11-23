// Win32 window + legacy OpenGL context for the GX backend.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <GL/gl.h>

#include "gx_internal.h"

static HWND hwnd;
static HDC hdc;
static HGLRC glrc;
static int win_w = 640 * 2, win_h = 480 * 2;
static int quit_requested;

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE) fprintf(stderr, "gx: close requested from the window menu / Alt+F4\n");
        return DefWindowProcA(h, msg, wp, lp);
    case WM_CLOSE: quit_requested = 1; return 0;
    case WM_SIZE: win_w = LOWORD(lp); win_h = HIWORD(lp); return 0;
    default: return DefWindowProcA(h, msg, wp, lp);
    }
}

int gx_win_open(void) {
    if (hwnd) return 1;
    WNDCLASSA wc = {0};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "BillyHatcherRecomp";
    RegisterClassA(&wc);
    RECT r = {0, 0, win_w, win_h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowA(wc.lpszClassName, "Billy Hatcher (recomp)", WS_OVERLAPPEDWINDOW,
                         CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL,
                         wc.hInstance, NULL);
    if (!hwnd) return 0;
    // GX_WINDOW=background: start minimized without stealing focus (automated runs)
    const char* mode = getenv("GX_WINDOW");
    ShowWindow(hwnd, (mode && !strcmp(mode, "background")) ? SW_SHOWMINNOACTIVE : SW_SHOW);
    hdc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                 PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 24, 8, 0,
                                 PFD_MAIN_PLANE, 0, 0, 0, 0};
    int pf = ChoosePixelFormat(hdc, &pfd);
    SetPixelFormat(hdc, pf, &pfd);
    glrc = wglCreateContext(hdc);
    wglMakeCurrent(hdc, glrc);
    fprintf(stderr, "gx: OpenGL %s (%s)\n", (const char*)glGetString(GL_VERSION),
            (const char*)glGetString(GL_RENDERER));
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    SwapBuffers(hdc);
    return 1;
}

void gx_win_pump(void) {
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    if (quit_requested) {
        fprintf(stderr, "boot: window closed\n");
        boot_print_stats();
        ExitProcess(0);
    }
}

void gx_win_size(int* w, int* h) { *w = win_w; *h = win_h; }

// Keyboard input is only read while the game window is in front.
int input_window_focused(void) { return hwnd && GetForegroundWindow() == hwnd; }

void gx_win_present(void) {
    SwapBuffers(hdc);
    gx_win_pump();
}

// Saves the current back buffer contents (before swap) as a BMP.
void gx_win_screenshot(const char* path) {
    int w = win_w, h = win_h;
    unsigned char* px = (unsigned char*)malloc((size_t)w * h * 3);
    if (!px) return;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_BGR_EXT, GL_UNSIGNED_BYTE, px);
    FILE* f = fopen(path, "wb");
    if (f) {
        int row = (w * 3 + 3) & ~3, size = 54 + row * h;
        unsigned char hdr[54] = {'B', 'M'};
        *(int*)(hdr + 2) = size; *(int*)(hdr + 10) = 54; *(int*)(hdr + 14) = 40;
        *(int*)(hdr + 18) = w; *(int*)(hdr + 22) = h; *(short*)(hdr + 26) = 1; *(short*)(hdr + 28) = 24;
        fwrite(hdr, 1, 54, f);
        unsigned char pad[3] = {0};
        for (int y = 0; y < h; y++) {
            fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f);
            fwrite(pad, 1, (size_t)(row - w * 3), f);
        }
        fclose(f);
    }
    free(px);
}
