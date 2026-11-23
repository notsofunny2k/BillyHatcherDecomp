// Minimal runtime for recompiled Gekko code: guest memory, DOL loading,
// indirect calls through the generated function table, error handlers.
#include "ppc_runtime.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

uint8_t* ppc_mem = NULL;

int ppc_mem_init(void) {
    ppc_mem = (uint8_t*)calloc(1, PPC_MEM_SIZE);
    return ppc_mem != NULL;
}

int ppc_load_dol(const char* path, uint32_t* entry) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* d = (uint8_t*)malloc((size_t)size);
    if (!d || fread(d, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(d); return 0; }
    fclose(f);
#define BE32(p) (((uint32_t)(p)[0] << 24) | ((uint32_t)(p)[1] << 16) | ((uint32_t)(p)[2] << 8) | (p)[3])
    for (int i = 0; i < 18; i++) {
        uint32_t off = BE32(d + 4 * i), addr = BE32(d + 0x48 + 4 * i), sz = BE32(d + 0x90 + 4 * i);
        if (sz && off + sz <= (uint32_t)size) memcpy(ppc_ptr(addr), d + off, sz);
    }
    uint32_t bss = BE32(d + 0xD8), bss_sz = BE32(d + 0xDC);
    // The bss range overlaps small data sections in the DOL; only clear the gaps.
    for (uint32_t a = bss; a < bss + bss_sz; a++) {
        int inside = 0;
        for (int i = 7; i < 18 && !inside; i++) {
            uint32_t addr = BE32(d + 0x48 + 4 * i), sz = BE32(d + 0x90 + 4 * i);
            if (sz && a >= addr && a < addr + sz) inside = 1;
        }
        if (!inside) *ppc_ptr(a) = 0;
    }
    if (entry) *entry = BE32(d + 0xE0);
#undef BE32
    free(d);
    return 1;
}

static void no_irq(int irq) { (void)irq; }
void (*ppc_raise_irq)(int irq) = no_irq;

volatile uint32_t ppc_trace_ring[1024];
volatile uint32_t ppc_trace_lr[1024];
volatile uint32_t ppc_trace_pos;

static int dyn_get(uint32_t addr, PPCFunc* fn, const char** name);

const char* ppc_func_name(uint32_t addr) {
    const char* rel_name;
    if ((addr < 0x80003000u || addr >= 0x80400000u) && dyn_get(addr, NULL, &rel_name)) return rel_name;
    size_t lo = 0, hi = ppc_func_table_size;
    while (lo < hi) {  // last entry with entry.addr <= addr
        size_t mid = (lo + hi) / 2;
        if (ppc_func_table[mid].addr <= addr) lo = mid + 1;
        else hi = mid;
    }
    return lo ? ppc_func_table[lo - 1].name : "?";
}

void ppc_dump_trace(int count) {
    // Walk back through the ring, collapsing runs of the same function.
    uint32_t pos = ppc_trace_pos;
    uint32_t avail = pos < 1024 ? pos : 1024;
    uint32_t addrs[64], lrs[64];
    int reps[64], n = 0;
    for (uint32_t i = 1; i <= avail && n < count && n < 64; i++) {
        uint32_t a = ppc_trace_ring[(pos - i) & 1023u], lr = ppc_trace_lr[(pos - i) & 1023u];
        if (n && addrs[n - 1] == a && lrs[n - 1] == lr) { reps[n - 1]++; continue; }
        addrs[n] = a; lrs[n] = lr; reps[n] = 1; n++;
    }
    fprintf(stderr, "--- last guest calls (oldest first, repeats collapsed) ---\n");
    for (int i = n - 1; i >= 0; i--) {
        fprintf(stderr, "  %08X %-28s <- %08X %s", addrs[i], ppc_func_name(addrs[i]), lrs[i],
                lrs[i] ? ppc_func_name(lrs[i]) : "");
        if (reps[i] > 1) fprintf(stderr, "  x%d", reps[i]);
        fprintf(stderr, "\n");
    }
}

void ppc_fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "ppc: fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    ppc_dump_trace(24);
    fflush(stderr);
    exit(3);
}

// ---- REL modules linked at runtime --------------------------------------
#define OS_MODULE_LIST_HEAD 0x800030C8u
#define DYN_SIZE 16384u
static struct { uint32_t addr; PPCFunc fn; const char* name; } dyn[DYN_SIZE];

static void dyn_put(uint32_t addr, PPCFunc fn, const char* name) {
    uint32_t i = (addr >> 2) * 2654435761u % DYN_SIZE;
    while (dyn[i].addr && dyn[i].addr != addr) i = (i + 1) % DYN_SIZE;
    dyn[i].addr = addr; dyn[i].fn = fn; dyn[i].name = name;
}

static int dyn_get(uint32_t addr, PPCFunc* fn, const char** name) {
    uint32_t i = (addr >> 2) * 2654435761u % DYN_SIZE;
    for (uint32_t n = 0; n < DYN_SIZE && dyn[i].addr; n++, i = (i + 1) % DYN_SIZE)
        if (dyn[i].addr == addr) { if (fn) *fn = dyn[i].fn; if (name) *name = dyn[i].name; return 1; }
    return 0;
}

// Walks the OS list of linked modules and (re)registers translated ones.
static int ppc_rel_resolve(void) {
    int changed = 0;
    for (uint32_t m = ld32(OS_MODULE_LIST_HEAD); m; m = ld32(m + 4)) {
        uint32_t id = ld32(m), nsec = ld32(m + 0xC), info = ld32(m + 0x10);
        for (size_t k = 0; k < ppc_rel_module_count; k++) {
            const PPCRelModule* mod = ppc_rel_modules[k];
            if (!mod || mod->id != id) continue;
            uint32_t text = ld32(info + 8 * mod->text_sec) & ~1u;
            if (mod->base[mod->text_sec] == text) continue;  // already registered here
            for (uint32_t s = 0; s < nsec && s < 32; s++) mod->base[s] = ld32(info + 8 * s) & ~1u;
            for (size_t i = 0; i < mod->count; i++)
                dyn_put(text + mod->funcs[i].offset, mod->funcs[i].fn, mod->funcs[i].name);
            fprintf(stderr, "ppc: REL module %u linked, code at %08X, %u functions registered\n", id, text,
                    (unsigned)mod->count);
            changed = 1;
        }
    }
    return changed;
}

PPCFunc ppc_lookup(uint32_t addr) {
    PPCFunc fn;
    if (addr < 0x80003000u || addr >= 0x80400000u) {  // outside the DOL: maybe a REL
        if (dyn_get(addr, &fn, NULL)) return fn;
        if (ppc_rel_resolve() && dyn_get(addr, &fn, NULL)) return fn;
        return NULL;
    }
    size_t lo = 0, hi = ppc_func_table_size;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (ppc_func_table[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo < ppc_func_table_size && ppc_func_table[lo].addr == addr) return ppc_func_table[lo].fn;
    return NULL;
}

void ppc_call_indirect(PPCContext* ctx, uint32_t addr) {
    PPCFunc fn = ppc_lookup(addr);
    if (!fn) ppc_fatal("indirect call to unknown address %08X (lr=%08X)", addr, ctx->lr);
    fn(ctx);
}

void ppc_unimplemented(PPCContext* ctx, uint32_t addr, uint32_t ins) {
    (void)ctx;
    ppc_fatal("unimplemented instruction %08X at %08X (%s)", ins, addr, ppc_func_name(addr));
}

void ppc_missing(PPCContext* ctx, uint32_t addr) {
    ppc_fatal("call to function %08X that was not recompiled (lr=%08X)", addr, ctx->lr);
}

void ppc_syscall(PPCContext* ctx) {
    // On GameCube `sc` is only used by PPCSync-style helpers; nothing to do.
    if (getenv("PPC_LOG_SC")) fprintf(stderr, "ppc: sc at lr=%08X\n", ctx->lr);
}

// Gekko time base runs at bus clock / 4 = 40.5 MHz; derive it from the
// host's high-resolution counter (cheap: this is called very often).
#ifdef _WIN32
#include <windows.h>
uint64_t ppc_get_timebase(void) {
    static LARGE_INTEGER t0, freq;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); t0 = now; }
    uint64_t d = (uint64_t)(now.QuadPart - t0.QuadPart);
    return d / (uint64_t)freq.QuadPart * 40500000ull + d % (uint64_t)freq.QuadPart * 40500000ull / (uint64_t)freq.QuadPart;
}
#else
uint64_t ppc_get_timebase(void) {
    static struct timespec t0;
    static int init;
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    if (!init) { t0 = now; init = 1; }
    uint64_t ns = (uint64_t)(now.tv_sec - t0.tv_sec) * 1000000000ull + (uint64_t)(now.tv_nsec - t0.tv_nsec);
    return ns * 81 / 2000;  // 40.5 ticks per microsecond
}
#endif

void ppc_ctx_init(PPCContext* ctx) {
    memset(ctx, 0, sizeof(*ctx));
}
