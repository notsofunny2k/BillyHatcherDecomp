// DVD interface (DI, 0xCC006000) backed by the disc image.
// Reads complete immediately and raise the DI interrupt, so the SDK's DVD
// layer (file system, queues, callbacks) runs translated on top of it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle_dvd.h"
#include "hle_irq.h"
#include "ppc_runtime.h"

enum {
    DISR = 0x00, DICVR = 0x04, DICMD0 = 0x08, DICMD1 = 0x0C, DICMD2 = 0x10,
    DIMAR = 0x14, DILEN = 0x18, DICR = 0x1C, DIIMM = 0x20, DICFG = 0x24,
};
#define DISR_TCINTMASK 0x08u
#define DISR_TCINT 0x10u
#define DISR_DEINT 0x04u
#define DISR_BRKINT 0x40u

static uint32_t di[0x40 / 4];
static FILE* iso;
static unsigned long long bytes_read, reads;

int hle_dvd_open(const char* path) {
    iso = fopen(path, "rb");
    return iso != NULL;
}

void hle_dvd_stats(void) {
    fprintf(stderr, "dvd: %llu reads, %llu bytes\n", reads, bytes_read);
}

static void complete(void) {
    di[DICR / 4] &= ~1u;  // TSTART done
    di[DILEN / 4] = 0;
    di[DISR / 4] |= DISR_TCINT;
    if (di[DISR / 4] & DISR_TCINTMASK) hle_irq_raise(IRQ_PI_DI);
}

static void execute(void) {
    uint32_t cmd = di[DICMD0 / 4], op = cmd >> 24;
    uint32_t dst = di[DIMAR / 4], len = di[DILEN / 4];
    switch (op) {
    case 0xA8: {  // read: CMD1 = disc offset >> 2, CMD2 = length
        uint64_t off = (uint64_t)di[DICMD1 / 4] << 2;
        uint8_t* p = ppc_ptr(0x80000000u | (dst & 0x01FFFFFFu));
        if (iso && len) {
            _fseeki64(iso, (long long)off, SEEK_SET);
            size_t got = fread(p, 1, len, iso);
            if (got < len) memset(p + got, 0, len - got);
            bytes_read += len;
            reads++;
        }
        if (getenv("PPC_LOG_DVD"))
            fprintf(stderr, "dvd: read %08llX len %X -> %08X\n", (unsigned long long)off, len, dst);
        break;
    }
    case 0x12: {  // inquiry: drive info (revision/date)
        uint8_t* p = ppc_ptr(0x80000000u | (dst & 0x01FFFFFFu));
        memset(p, 0, len < 0x20 ? len : 0x20);
        if (len >= 8) { p[2] = 0x00; p[3] = 0x02; p[4] = 0x20; p[5] = 0x02; p[6] = 0x04; p[7] = 0x02; }
        break;
    }
    case 0xE0:  // request error: no error
        di[DIIMM / 4] = 0;
        break;
    default:  // seek, stop motor, audio stream config...: nothing to do
        di[DIIMM / 4] = 0;
        break;
    }
    complete();
}

static int write_hook(uint32_t off, uint32_t v, int size) {
    if (off < 0x6000 || off >= 0x6040) return 0;
    uint32_t r = (off - 0x6000) & ~3u;
    if (size != 4) {  // merge 16/8-bit writes into the 32-bit register
        int shift = (int)(8 * (4 - size - (int)((off - 0x6000) & 3)));
        uint32_t mask = (size == 2 ? 0xFFFFu : 0xFFu) << shift;
        v = (di[r / 4] & ~mask) | ((v << shift) & mask);
    }
    if (r == DISR) {
        // interrupt bits are write-one-to-clear, mask bits are plain
        uint32_t ints = DISR_TCINT | DISR_DEINT | DISR_BRKINT;
        di[DISR / 4] = (v & ~ints) | (di[DISR / 4] & ints & ~v);
        return 1;
    }
    if (r == DICVR) {
        di[DICVR / 4] = (v & ~0x4u) | (di[DICVR / 4] & 0x4u & ~v);
        return 1;
    }
    di[r / 4] = v;
    if (r == DICR && (v & 1)) execute();
    return 1;
}

static int read_hook(uint32_t off, int size, uint32_t* out) {
    if (off < 0x6000 || off >= 0x6040) return 0;
    uint32_t r = (off - 0x6000) & ~3u, v = di[r / 4];
    if (size != 4) {
        int shift = (int)(8 * (4 - size - (int)((off - 0x6000) & 3)));
        v = (v >> shift) & (size == 2 ? 0xFFFFu : 0xFFu);
    }
    *out = v;
    return 1;
}

void hle_dvd_install(void) {
    ppc_mmio_write_hook = write_hook;
    ppc_mmio_read_hook = read_hook;
}
