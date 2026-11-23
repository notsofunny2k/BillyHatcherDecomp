// Hardware register access (0xCC00xxxx). For now a plain register file:
// writes are stored, reads return the last written value. Every register
// touched is logged once so missing device behaviour can be added.
#include <stdio.h>
#include <stdlib.h>

#include "ppc_runtime.h"

static uint8_t regs[0x10000];
static uint8_t seen[0x10000];

static const char* block_name(uint32_t off) {
    switch (off >> 12) {
    case 0x0: return "CP";
    case 0x1: return "PE";
    case 0x2: return "VI";
    case 0x3: return "PI";
    case 0x4: return "MI";
    case 0x5: return "DSP";
    case 0x6: return (off & 0x0F00) == 0x0000 ? "DI" : (off & 0x0F00) == 0x0400 ? "SI"
                   : (off & 0x0F00) == 0x0800 ? "EXI" : (off & 0x0F00) == 0x0C00 ? "AI" : "?";
    case 0x8: return "GXFIFO";
    default: return "?";
    }
}

static void note(uint32_t a, int size, int write, uint32_t v) {
    uint32_t off = a & 0xFFFF;
    if (seen[off] & (write ? 2 : 1)) return;
    seen[off] |= (uint8_t)(write ? 2 : 1);
    if (getenv("PPC_LOG_MMIO"))
        fprintf(stderr, "mmio %s %s%d %08X = %08X (%s)\n", block_name(off), write ? "W" : "R", size * 8, a, v,
                ppc_func_name(ppc_trace_ring[(ppc_trace_pos - 1) & 1023u]));
}

uint32_t ppc_mmio_read(uint32_t a, int size) {
    uint32_t off = a & 0xFFFF, v = 0;
    if (ppc_mmio_read_hook && ppc_mmio_read_hook(off, size, &v)) { note(a, size, 0, v); return v; }
    for (int i = 0; i < size; i++) v = (v << 8) | regs[(off + (uint32_t)i) & 0xFFFF];
    if (off == 0x5016 && size == 2) v |= 1;  // ARAM mode: controller ready
    note(a, size, 0, v);
    return v;
}

static uint16_t reg16(uint32_t off);
static void set16(uint32_t off, uint16_t v);

int (*ppc_mmio_write_hook)(uint32_t off, uint32_t v, int size);
int (*ppc_mmio_read_hook)(uint32_t off, int size, uint32_t* v);

// Lets device code (VI timing, etc.) change register values directly.
uint16_t ppc_mmio_get16(uint32_t off) { return reg16(off & 0xFFFF); }
void ppc_mmio_set16(uint32_t off, uint16_t v) { set16(off & 0xFFFF, v); }

// ---- ARAM (16 MB auxiliary RAM) and its DMA engine ----------------------
#define ARAM_SIZE 0x01000000u
uint8_t* ppc_aram;

static uint16_t reg16(uint32_t off) { return (uint16_t)((regs[off] << 8) | regs[off + 1]); }
static void set16(uint32_t off, uint16_t v) { regs[off] = (uint8_t)(v >> 8); regs[off + 1] = (uint8_t)v; }

static void aram_dma(void) {
    if (!ppc_aram) ppc_aram = (uint8_t*)calloc(1, ARAM_SIZE);
    uint32_t mm = ((uint32_t)reg16(0x5020) << 16 | reg16(0x5022)) & 0x03FFFFE0u;
    uint32_t ar = ((uint32_t)reg16(0x5024) << 16 | reg16(0x5026)) & 0x03FFFFE0u;
    uint32_t cnt_h = reg16(0x5028), len = ((cnt_h & 0x7FFF) << 16 | reg16(0x502A)) & ~0x1Fu;
    int to_main = (cnt_h & 0x8000) != 0;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t a = (ar + i) % ARAM_SIZE;
        uint8_t* m = ppc_ptr(0x80000000u | ((mm + i) & 0x01FFFFFFu));
        if (to_main) *m = ppc_aram[a];
        else ppc_aram[a] = *m;
    }
    set16(0x5028, 0);
    set16(0x502A, 0);
    // DMA finished: clear "ARAM DMA busy" (0x200), raise ARINT (0x20)
    uint16_t csr = (uint16_t)((reg16(0x500A) & ~0x200u) | 0x20u);
    set16(0x500A, csr);
    if (csr & 0x40) ppc_raise_irq(6);  // ARINTMSK set: __OS_INTERRUPT_DSP_ARAM
}

// DSP control/status: interrupt bits are write-one-to-clear.
static void write_dsp_csr(uint16_t v) {
    const uint16_t ints = 0x08 | 0x20 | 0x80;  // AIDINT, ARINT, DSPINT
    uint16_t old = reg16(0x500A);
    uint16_t nv = (uint16_t)((v & ~ints) | (old & ints & ~v));
    nv &= (uint16_t)~0x0001;  // reset bit self-clears
    set16(0x500A, nv);
}

// Transfer-start bits that real hardware clears when the transfer finishes.
// With no devices attached every transfer completes immediately.
static uint32_t complete_transfers(uint32_t off, uint32_t v) {
    if (off >= 0x6800 && off < 0x6800 + 3 * 0x14 && ((off - 0x6800) % 0x14) == 0x0C)
        return v & ~1u;                        // EXI channel control: TSTART
    if (off == 0x6434) return v & ~1u;        // SI comm control: TSTART
    if (off == 0x601C) return v & ~1u;        // DI control: TSTART
    return v;
}

void ppc_mmio_write(uint32_t a, uint32_t v, int size) {
    uint32_t off = a & 0xFFFF;
    if (ppc_mmio_write_hook && ppc_mmio_write_hook(off, v, size)) return;  // devices (DVD, GX FIFO...)
    if ((off >> 12) == 0x8) return;  // GX FIFO without a graphics backend: dropped
    note(a, size, 1, v);
    if (size == 4) v = complete_transfers(off, v);
    if (off == 0x500A && size == 2) { write_dsp_csr((uint16_t)v); return; }
    // CPU->DSP mailbox: there is no DSP, so mail is "read" immediately
    // (clears the pending bit that DSPCheckMailToDSP polls).
    if (off == 0x5000 && size == 4) { set16(0x5000, (uint16_t)((v >> 16) & 0x7FFF)); set16(0x5002, (uint16_t)v); return; }
    if (off == 0x5000 && size == 2) { set16(0x5000, (uint16_t)(v & 0x7FFF)); return; }
    if (off == 0x5008 && size == 4) {  // 32-bit write covering the CSR's low half
        set16(0x5008, (uint16_t)(v >> 16));
        write_dsp_csr((uint16_t)v);
        return;
    }
    uint32_t v0 = v;
    for (int i = size - 1; i >= 0; i--) { regs[(off + (uint32_t)i) & 0xFFFF] = (uint8_t)v; v >>= 8; }
    (void)v0;
    if ((off == 0x502A && size == 2) || (off == 0x5028 && size == 4)) aram_dma();
}
