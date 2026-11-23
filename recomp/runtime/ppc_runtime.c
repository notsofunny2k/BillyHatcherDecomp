// Minimal runtime for recompiled Gekko code: guest memory, DOL loading,
// indirect calls through the generated function table, error handlers.
#include "ppc_runtime.h"

#include <stdio.h>
#include <stdlib.h>

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

PPCFunc ppc_lookup(uint32_t addr) {
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
    if (!fn) {
        fprintf(stderr, "ppc: indirect call to unknown address %08X (lr=%08X)\n", addr, ctx->lr);
        abort();
    }
    fn(ctx);
}

void ppc_unimplemented(PPCContext* ctx, uint32_t addr, uint32_t ins) {
    (void)ctx;
    fprintf(stderr, "ppc: unimplemented instruction %08X at %08X\n", ins, addr);
    abort();
}

void ppc_missing(PPCContext* ctx, uint32_t addr) {
    fprintf(stderr, "ppc: call to function %08X that was not recompiled (lr=%08X)\n", addr, ctx->lr);
    abort();
}

void ppc_syscall(PPCContext* ctx) {
    fprintf(stderr, "ppc: sc at lr=%08X\n", ctx->lr);
}

uint64_t ppc_get_timebase(void) {
    static uint64_t tb = 0;
    return tb += 1000;
}

void ppc_ctx_init(PPCContext* ctx) {
    memset(ctx, 0, sizeof(*ctx));
}
