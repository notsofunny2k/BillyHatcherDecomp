// Sampling profiler (PPC_PROFILE=1): every millisecond the main thread is
// suspended and its instruction pointer is mapped to the translated guest
// function containing it (each guest function is one host C function).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "ppc_runtime.h"

typedef struct { uintptr_t host; uint32_t guest; const char* name; unsigned long long samples; } Entry;

static Entry* table;
static size_t n;
static HANDLE main_thread;
static volatile unsigned long long total, outside;
static volatile int running;

static int by_host(const void* a, const void* b) {
    uintptr_t x = ((const Entry*)a)->host, y = ((const Entry*)b)->host;
    return x < y ? -1 : x > y;
}
static int by_samples(const void* a, const void* b) {
    unsigned long long x = ((const Entry*)a)->samples, y = ((const Entry*)b)->samples;
    return x < y ? 1 : x > y ? -1 : 0;
}

static DWORD WINAPI sampler(LPVOID arg) {
    (void)arg;
    timeBeginPeriod(1);
    while (running) {
        Sleep(1);
        if (SuspendThread(main_thread) == (DWORD)-1) continue;
        CONTEXT c;
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(main_thread, &c)) {
            uintptr_t ip = (uintptr_t)c.Rip;
            size_t lo = 0, hi = n;
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                if (table[mid].host <= ip) lo = mid + 1;
                else hi = mid;
            }
            total++;
            // inside generated code if below the next function and within 64 KB of the start
            if (lo && ip - table[lo - 1].host < 0x10000) table[lo - 1].samples++;
            else outside++;
        }
        ResumeThread(main_thread);
    }
    return 0;
}

void profiler_start(void) {
    if (!getenv("PPC_PROFILE")) return;
    n = ppc_func_table_size;
    table = (Entry*)calloc(n, sizeof(Entry));
    for (size_t i = 0; i < n; i++) {
        table[i].host = (uintptr_t)ppc_func_table[i].fn;
        table[i].guest = ppc_func_table[i].addr;
        table[i].name = ppc_func_table[i].name;
    }
    qsort(table, n, sizeof(Entry), by_host);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    running = 1;
    CreateThread(NULL, 0, sampler, NULL, 0, NULL);
}

void profiler_report(void) {
    if (!table || !total) return;
    running = 0;
    qsort(table, n, sizeof(Entry), by_samples);
    fprintf(stderr, "--- profile: %llu samples, %.1f%% outside translated DOL code (runtime/GL/REL/idle) ---\n",
            total, 100.0 * (double)outside / (double)total);
    for (size_t i = 0; i < 30 && i < n && table[i].samples; i++)
        fprintf(stderr, "  %5.1f%%  %08X %s\n", 100.0 * (double)table[i].samples / (double)total, table[i].guest,
                table[i].name);
}
