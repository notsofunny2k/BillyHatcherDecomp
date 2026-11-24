// EXI bus (0xCC006800) with a memory card in slot A (channel 0, device 0).
//
// The card is a raw 2 MB image (251 blocks, same layout as Dolphin's .raw
// memory card files). The whole CARD library runs translated on top of the
// emulated device protocol: ID (0x00), status (0x83), clear status (0x89),
// vendor ID (0x85), interrupt enable (0x81), read (0x52), page program (0xF2),
// sector erase (0xF1), chip erase (0xF4). Only the security unlock, which
// needs the DSP, is replaced natively (hle___CARDUnlock).
//
// Other EXI devices (RTC/SRAM, IPL, ...) read as zero, as before.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <direct.h>

#include "hle_irq.h"
#include "ppc_runtime.h"

#define CARD_SIZE (2u * 1024u * 1024u)  // 16 Mbit
#define CARD_ID 0x00000010u              // size code 16 (Mbit), 8 KB sectors, latency index 0
#define SECTOR 0x2000u

enum { CSR = 0, MAR = 1, LEN = 2, CR = 3, DATA = 4 };
static uint32_t exi[3][5];

typedef struct {
    int selected;
    uint8_t cmd[16];
    int cmdlen, need;
    uint32_t addr, pos;
    uint32_t dirty_lo, dirty_hi;
    int unlocked, int_enabled, present;
    uint8_t* image;
    FILE* file;
} Card;
static Card card;
static unsigned long long reads, writes, erases;

static int header_len(uint8_t c) {
    switch (c) {
    case 0x52: case 0xF2: return 5;
    case 0xF1: case 0xF4: return 3;
    case 0x89: return 1;
    default: return 2;  // 0x00 ID, 0x83 status, 0x85 vendor, 0x81 int enable
    }
}

static uint32_t decode_addr(void) {
    return ((uint32_t)(card.cmd[1] & 0x7F) << 17) | ((uint32_t)card.cmd[2] << 9) | ((uint32_t)(card.cmd[3] & 3) << 7) |
           (uint32_t)(card.cmd[4] & 0x7F);
}

static void card_open(void) {
    if (card.image) return;
    const char* path = getenv("PPC_MEMCARD_A");
    if (!path) { _mkdir("build"); _mkdir("build/memcard"); path = "build/memcard/MemoryCardA.raw"; }
    card.image = (uint8_t*)malloc(CARD_SIZE);
    memset(card.image, 0xFF, CARD_SIZE);
    FILE* f = fopen(path, "rb");
    if (f) { fread(card.image, 1, CARD_SIZE, f); fclose(f); }
    card.file = fopen(path, f ? "r+b" : "w+b");
    if (card.file && !f) { fwrite(card.image, 1, CARD_SIZE, card.file); fflush(card.file); }
    fprintf(stderr, "exi: memory card A = %s (%s)\n", path, f ? "loaded" : "new, unformatted");
    card.present = 1;
}

static void persist(uint32_t lo, uint32_t hi) {
    if (!card.file || hi <= lo) return;
    fseek(card.file, (long)lo, SEEK_SET);
    fwrite(card.image + lo, 1, hi - lo, card.file);
    fflush(card.file);
}

static void card_select(void) {
    card.selected = 1;
    card.cmdlen = 0;
    card.need = 1;
    card.pos = 0;
    card.dirty_lo = CARD_SIZE;
    card.dirty_hi = 0;
}

static void raise_exi_int(void) {
    exi[0][CSR] |= 0x2;  // EXIINT (delivery is gated by the OS interrupt mask)
    hle_irq_raise(IRQ_EXI0_EXI);
}

static void card_deselect(void) {
    if (!card.selected) return;
    card.selected = 0;
    if (getenv("EXI_LOG")) {
        fprintf(stderr, "exi: card cmd");
        for (int i = 0; i < card.cmdlen && i < 5; i++) fprintf(stderr, " %02X", card.cmd[i]);
        fprintf(stderr, "  (%u data bytes)\n", card.pos);
    }
    if (card.cmdlen < card.need) return;
    switch (card.cmd[0]) {
    case 0xF2:  // page program done
        persist(card.dirty_lo, card.dirty_hi);
        writes++;
        raise_exi_int();
        break;
    case 0xF1: {  // sector erase
        uint32_t a = (((uint32_t)(card.cmd[1] & 0x7F) << 17) | ((uint32_t)card.cmd[2] << 9)) & ~(SECTOR - 1);
        if (a < CARD_SIZE) { memset(card.image + a, 0xFF, SECTOR); persist(a, a + SECTOR); }
        erases++;
        raise_exi_int();
        break;
    }
    case 0xF4:  // chip erase
        memset(card.image, 0xFF, CARD_SIZE);
        persist(0, CARD_SIZE);
        erases++;
        raise_exi_int();
        break;
    case 0x81: card.int_enabled = card.cmd[1] & 1; break;
    default: break;
    }
}

static void card_write_byte(uint8_t b) {
    if (card.cmdlen < card.need) {
        card.cmd[card.cmdlen++] = b;
        if (card.cmdlen == 1) card.need = header_len(b);
        if (card.cmdlen == card.need && (card.cmd[0] == 0x52 || card.cmd[0] == 0xF2)) {
            card.addr = decode_addr();
            card.pos = 0;
        }
        return;
    }
    if (card.cmd[0] == 0xF2) {  // page data
        uint32_t a = (card.addr + card.pos++) % CARD_SIZE;
        card.image[a] = b;
        if (a < card.dirty_lo) card.dirty_lo = a;
        if (a + 1 > card.dirty_hi) card.dirty_hi = a + 1;
    }
    // 0x52: latency dummy bytes after the header are ignored
}

static uint8_t card_read_byte(void) {
    uint32_t p = card.pos++;
    switch (card.cmd[0]) {
    case 0x00: return p < 4 ? (uint8_t)(CARD_ID >> (24 - 8 * p)) : 0;
    case 0x83: return (uint8_t)(0x01 | (card.unlocked ? 0x40 : 0));  // ready (+ unlocked)
    case 0x85: return 0;                                             // vendor ID 0x0000
    case 0x52:
        if (p == 0) reads++;
        return card.image[(card.addr + p) % CARD_SIZE];
    default: return 0;
    }
}

static int card_on(int ch) { return ch == 0 && card.present && card.selected && ((exi[0][CSR] >> 7) & 1); }

static void transfer(int ch) {
    uint32_t c = exi[ch][CR];
    int dma = (c >> 1) & 1, rw = (c >> 2) & 3;
    int dev = card_on(ch);
    if (!dma) {
        int n = (int)((c >> 4) & 3) + 1;
        uint32_t in = exi[ch][DATA], out = 0;
        for (int i = 0; i < n; i++) {
            uint8_t wb = (uint8_t)(in >> (24 - 8 * i)), rb = 0;
            if (dev && (rw == 1 || rw == 2)) card_write_byte(wb);
            if (dev && (rw == 0 || rw == 2)) rb = card_read_byte();
            out |= (uint32_t)rb << (24 - 8 * i);
        }
        if (rw != 1) exi[ch][DATA] = out;
    } else {
        uint32_t mar = exi[ch][MAR] & 0x03FFFFE0u, len = exi[ch][LEN] & 0x03FFFFE0u;
        for (uint32_t i = 0; i < len; i++) {
            uint8_t* m = ppc_ptr(0x80000000u | ((mar + i) & 0x01FFFFFFu));
            if (rw == 1) { if (dev) card_write_byte(*m); }
            else *m = dev ? card_read_byte() : 0;
        }
    }
    exi[ch][CR] &= ~1u;     // TSTART done
    exi[ch][CSR] |= 0x8;    // TCINT
    hle_irq_raise(IRQ_EXI0_TC + 3 * ch);  // gated by the OS interrupt mask
}

static int (*next_write)(uint32_t, uint32_t, int);
static int (*next_read)(uint32_t, int, uint32_t*);

static int write_hook(uint32_t off, uint32_t v, int size) {
    if (off < 0x6800 || off >= 0x6800 + 3 * 0x14) return next_write ? next_write(off, v, size) : 0;
    int ch = (int)((off - 0x6800) / 0x14), r = (int)(((off - 0x6800) % 0x14) / 4);
    if (size != 4) {  // merge partial writes
        int shift = (int)(8 * (4 - size - (int)((off - 0x6800) % 4)));
        uint32_t mask = (size == 2 ? 0xFFFFu : 0xFFu) << shift;
        v = (exi[ch][r] & ~mask) | ((v << shift) & mask);
    }
    if (r == CSR) {
        uint32_t ints = 0x2 | 0x8 | 0x800;  // EXIINT, TCINT, EXTINT: write-one-to-clear
        uint32_t old = exi[ch][CSR];
        uint32_t nv = (v & ~ints & ~0x1000u) | (old & ints & ~v);
        if (ch == 0 && card.present) nv |= 0x1000;  // EXT: card inserted
        exi[ch][CSR] = nv;
        if (ch == 0 && card.present) {
            int was = (old >> 7) & 1, now = (nv >> 7) & 1;
            if (!was && now) card_select();
            if (was && !now) card_deselect();
        }
        return 1;
    }
    exi[ch][r] = v;
    if (r == CR && (v & 1)) transfer(ch);
    return 1;
}

static int read_hook(uint32_t off, int size, uint32_t* out) {
    if (off < 0x6800 || off >= 0x6800 + 3 * 0x14) return next_read ? next_read(off, size, out) : 0;
    int ch = (int)((off - 0x6800) / 0x14), r = (int)(((off - 0x6800) % 0x14) / 4);
    uint32_t v = exi[ch][r];
    if (r == CSR && ch == 0 && card.present) v |= 0x1000;
    if (size != 4) {
        int shift = (int)(8 * (4 - size - (int)((off - 0x6800) % 4)));
        v = (v >> shift) & (size == 2 ? 0xFFFFu : 0xFFu);
    }
    *out = v;
    return 1;
}

void exi_install(void) {
    if (!getenv("PPC_NO_MEMCARD")) card_open();
    next_write = ppc_mmio_write_hook;
    next_read = ppc_mmio_read_hook;
    ppc_mmio_write_hook = write_hook;
    ppc_mmio_read_hook = read_hook;
}

void exi_stats(void) {
    if (card.present)
        fprintf(stderr, "exi: memory card: %llu reads, %llu page writes, %llu erases, %s\n", reads, writes, erases,
                card.unlocked ? "unlocked" : "locked");
}

// s32 __CARDUnlock(s32 chan, u8 flashID[12]): the real one runs a DSP task to
// answer the card's challenge. Here the card simply becomes unlocked; the
// mount continues through __CARDMountCallback(chan, CARD_RESULT_READY), which
// is what the DSP task's completion would call.
void hle___CARDUnlock(PPCContext* ctx) {
    uint32_t chan = ctx->r[3], flash = ctx->r[4];
    for (int i = 0; i < 12; i++) st8(flash + (uint32_t)i, 0);
    card.unlocked = 1;
    static uint32_t mount_cb;
    if (!mount_cb) mount_cb = hle_find_function("__CARDMountCallback");
    if (mount_cb) hle_defer_call(mount_cb, chan, 0, 0);
    ctx->r[3] = 0;  // CARD_RESULT_READY
}
