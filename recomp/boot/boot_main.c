// Boot harness: prepares guest memory like the GameCube IPL/apploader would,
// then runs the translated entry point (__start).
//
// Usage: boot.exe <main.dol> <extracted sys dir> [timeout_seconds]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "hle_dvd.h"
#include "hle_irq.h"
#include "ppc_funcs.h"
#include "ppc_runtime.h"

static PPCContext ctx;

static uint8_t* read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* d = (uint8_t*)malloc((size_t)n);
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (size) *size = (size_t)n;
    return d;
}

static void load_into(uint32_t addr, const uint8_t* d, size_t n) { memcpy(ppc_ptr(addr), d, n); }

static int setup_low_memory(const char* sysdir) {
    char path[1024];
    size_t boot_n, fst_n, bi2_n;
    snprintf(path, sizeof(path), "%s/boot.bin", sysdir);
    uint8_t* boot = read_file(path, &boot_n);
    snprintf(path, sizeof(path), "%s/fst.bin", sysdir);
    uint8_t* fst = read_file(path, &fst_n);
    snprintf(path, sizeof(path), "%s/bi2.bin", sysdir);
    uint8_t* bi2 = read_file(path, &bi2_n);
    if (!boot || !fst || !bi2) {
        fprintf(stderr, "boot: missing boot.bin / fst.bin / bi2.bin in %s\n", sysdir);
        return 0;
    }
    // Disc ID (game code, maker, disc number, version...)
    load_into(0x80000000u, boot, 0x20);
    // FST and BI2 at the top of main RAM, like the apploader leaves them.
    uint32_t fst_addr = (0x81800000u - (uint32_t)fst_n) & ~0x1Fu;
    uint32_t bi2_addr = (fst_addr - (uint32_t)bi2_n) & ~0x1Fu;
    load_into(fst_addr, fst, fst_n);
    load_into(bi2_addr, bi2, bi2_n);

    st32(0x80000020u, 0x0D15EA5Eu);   // boot magic (normal boot)
    st32(0x80000024u, 1);             // boot info version
    st32(0x80000028u, 0x01800000u);   // physical memory size
    st32(0x8000002Cu, 0x00000001u);   // console type: retail
    st32(0x80000030u, 0);             // arena lo: let OS use __ArenaLo
    st32(0x80000034u, bi2_addr);      // arena hi
    st32(0x80000038u, fst_addr);      // FST location
    st32(0x8000003Cu, (uint32_t)fst_n);  // FST max size
    st32(0x800000CCu, 0);             // VI: NTSC
    st32(0x800000F0u, 0x01800000u);   // simulated memory size
    st32(0x800000F4u, bi2_addr);      // BI2 location
    st32(0x800000F8u, 162000000u);    // bus clock
    st32(0x800000FCu, 486000000u);    // CPU clock
    free(boot); free(fst); free(bi2);
    return 1;
}

static volatile int g_timeout = 20;

static DWORD WINAPI watchdog(LPVOID arg) {
    (void)arg;
    uint32_t last = ppc_trace_pos;
    int still = 0, elapsed = 0;
    for (;;) {
        Sleep(1000);
        elapsed++;
        uint32_t now = ppc_trace_pos;
        still = (now == last) ? still + 1 : 0;
        last = now;
        if (still >= 3) {
            fprintf(stderr, "boot: no new guest calls for 3 s (stuck in a loop?)\n");
            ppc_dump_trace(32);
            hle_dvd_stats();
            fflush(stderr);
            ExitProcess(4);
        }
        if (elapsed >= g_timeout) {
            fprintf(stderr, "boot: timeout after %d s (guest still running)\n", elapsed);
            ppc_dump_trace(32);
            hle_dvd_stats();
            fflush(stderr);
            ExitProcess(5);
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <main.dol> <sys dir> <disc.iso> [timeout]\n", argv[0]);
        return 2;
    }
    if (argc > 4) g_timeout = atoi(argv[4]);
    if (!hle_dvd_open(argv[3])) {
        fprintf(stderr, "boot: could not open disc image %s\n", argv[3]);
        return 2;
    }
    hle_dvd_install();
    uint32_t entry = 0;
    if (!ppc_mem_init() || !ppc_load_dol(argv[1], &entry)) {
        fprintf(stderr, "boot: could not load %s\n", argv[1]);
        return 2;
    }
    if (!setup_low_memory(argv[2])) return 2;
    ppc_ctx_init(&ctx);
    ctx.msr = 0x00002032u;  // FP available, IR/DR on, like after the IPL

    ppc_raise_irq = hle_irq_raise;
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    fprintf(stderr, "boot: entry %08X (%s)\n", entry, ppc_func_name(entry));
    ppc_call_indirect(&ctx, entry);
    fprintf(stderr, "boot: entry point returned (r3=%08X)\n", ctx.r[3]);
    ppc_dump_trace(16);
    return 0;
}
