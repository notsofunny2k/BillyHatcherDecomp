// Semantic tests for recompiled functions: run the translated guest code on
// known inputs and compare against results computed natively.
//
// Build (see recomp/run_tests.py):
//   ppc2c.py --only <names> --closure -> build/recomp_test, then compile with this file.
// Run:   test_basic.exe orig/GEZE8P/sys/main.dol
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "ppc_funcs.h"
#include "ppc_runtime.h"

static PPCContext ctx;
static int failures, checks;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            failures++;                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

static void call(uint32_t addr) {
    PPCFunc fn = ppc_lookup(addr);
    if (!fn) {
        printf("missing function %08X\n", addr);
        exit(2);
    }
    ctx.lr = 0;
    fn(&ctx);
}

#define SCRATCH 0x81000000u
#define SCRATCH2 0x81001000u
#define SCRATCH3 0x81002000u

static void put_str(uint32_t a, const char* s) {
    do { st8(a++, (uint8_t)*s); } while (*s++);
}

static void test_mem(void) {
    // memset
    for (int i = 0; i < 256; i++) st8(SCRATCH + i, 0x11);
    ctx.r[3] = SCRATCH + 3; ctx.r[4] = 0xAB; ctx.r[5] = 100;
    call(PPC_ADDR_memset);
    int ok = 1;
    for (int i = 0; i < 256; i++) {
        uint8_t want = (i >= 3 && i < 103) ? 0xAB : 0x11;
        if (ld8(SCRATCH + i) != want) ok = 0;
    }
    CHECK(ok, "memset contents");
    CHECK(ctx.r[3] == SCRATCH + 3, "memset return %08X", ctx.r[3]);

    // memcpy (unaligned, odd length)
    for (int i = 0; i < 200; i++) { st8(SCRATCH2 + i, (uint8_t)(i * 7 + 1)); st8(SCRATCH + i, 0); }
    ctx.r[3] = SCRATCH + 1; ctx.r[4] = SCRATCH2 + 2; ctx.r[5] = 77;
    call(PPC_ADDR_memcpy);
    ok = 1;
    for (int i = 0; i < 77; i++) if (ld8(SCRATCH + 1 + i) != (uint8_t)((i + 2) * 7 + 1)) ok = 0;
    CHECK(ok && ld8(SCRATCH) == 0 && ld8(SCRATCH + 78) == 0, "memcpy contents");

    // strlen
    put_str(SCRATCH, "hello, world");
    ctx.r[3] = SCRATCH;
    call(PPC_ADDR_strlen);
    CHECK(ctx.r[3] == 12, "strlen = %u", ctx.r[3]);

    // memcmp
    put_str(SCRATCH, "abcdef");
    put_str(SCRATCH2, "abcxef");
    ctx.r[3] = SCRATCH; ctx.r[4] = SCRATCH2; ctx.r[5] = 6;
    call(PPC_ADDR_memcmp);
    CHECK((int32_t)ctx.r[3] < 0, "memcmp less = %d", (int32_t)ctx.r[3]);
    ctx.r[3] = SCRATCH; ctx.r[4] = SCRATCH2; ctx.r[5] = 3;
    call(PPC_ADDR_memcmp);
    CHECK(ctx.r[3] == 0, "memcmp equal = %d", (int32_t)ctx.r[3]);

    // strncpy (pads with zeros)
    for (int i = 0; i < 16; i++) st8(SCRATCH3 + i, 0xEE);
    put_str(SCRATCH, "abc");
    ctx.r[3] = SCRATCH3; ctx.r[4] = SCRATCH; ctx.r[5] = 8;
    call(PPC_ADDR_strncpy);
    ok = ld8(SCRATCH3) == 'a' && ld8(SCRATCH3 + 2) == 'c' && ld8(SCRATCH3 + 7) == 0 && ld8(SCRATCH3 + 8) == 0xEE;
    CHECK(ok, "strncpy contents");
}

static void test_int64(void) {
    const int64_t vals[] = {0, 1, -1, 7, -7, 123456789012345LL, -98765432109876LL,
                            0x7FFFFFFFFFFFLL, (int64_t)0x8000000000000001ULL, 1000000007LL, 3};
    int n = (int)(sizeof(vals) / sizeof(vals[0]));
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            int64_t a = vals[i], b = vals[j];
            if (b == 0) continue;
            ctx.r[3] = (uint32_t)((uint64_t)a >> 32); ctx.r[4] = (uint32_t)a;
            ctx.r[5] = (uint32_t)((uint64_t)b >> 32); ctx.r[6] = (uint32_t)b;
            call(PPC_ADDR___div2i);
            int64_t q = (int64_t)(((uint64_t)ctx.r[3] << 32) | ctx.r[4]);
            CHECK(q == a / b, "__div2i(%lld, %lld) = %lld", (long long)a, (long long)b, (long long)q);
            ctx.r[3] = (uint32_t)((uint64_t)a >> 32); ctx.r[4] = (uint32_t)a;
            ctx.r[5] = (uint32_t)((uint64_t)b >> 32); ctx.r[6] = (uint32_t)b;
            call(PPC_ADDR___mod2i);
            int64_t r = (int64_t)(((uint64_t)ctx.r[3] << 32) | ctx.r[4]);
            CHECK(r == a % b, "__mod2i(%lld, %lld) = %lld", (long long)a, (long long)b, (long long)r);
        }
    for (int s = 0; s < 64; s += 5) {
        uint64_t a = 0x0123456789ABCDEFull;
        ctx.r[3] = (uint32_t)(a >> 32); ctx.r[4] = (uint32_t)a; ctx.r[5] = (uint32_t)s;
        call(PPC_ADDR___shl2i);
        uint64_t r = ((uint64_t)ctx.r[3] << 32) | ctx.r[4];
        CHECK(r == a << s, "__shl2i(%d) = %016llX", s, (unsigned long long)r);
    }
}

static void put_mtx(uint32_t a, const float m[12]) { for (int i = 0; i < 12; i++) stf32(a + 4 * i, m[i]); }
static void get_mtx(uint32_t a, float m[12]) { for (int i = 0; i < 12; i++) m[i] = ldf32(a + 4 * i); }

static void test_mtx(void) {
    float m[12], a[12], b[12], ab[12];
    ctx.r[3] = SCRATCH;
    call(PPC_ADDR_PSMTXIdentity);
    get_mtx(SCRATCH, m);
    int ok = 1;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++)
            if (m[r * 4 + c] != (r == c ? 1.0f : 0.0f)) ok = 0;
    CHECK(ok, "PSMTXIdentity");

    for (int i = 0; i < 12; i++) { a[i] = (float)(i + 1) * 0.5f - 2.0f; b[i] = (float)((i * 5) % 7) - 3.25f; }
    put_mtx(SCRATCH, a); put_mtx(SCRATCH2, b);
    ctx.r[3] = SCRATCH; ctx.r[4] = SCRATCH2; ctx.r[5] = SCRATCH3;
    call(PPC_ADDR_PSMTXConcat);
    get_mtx(SCRATCH3, ab);
    ok = 1;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) {
            double want = (c == 3) ? a[r * 4 + 3] : 0.0;
            for (int k = 0; k < 3; k++) want += (double)a[r * 4 + k] * b[k * 4 + c];
            if (fabs(want - ab[r * 4 + c]) > 1e-4) ok = 0;
        }
    CHECK(ok, "PSMTXConcat");

    ctx.r[3] = SCRATCH; ctx.r[4] = SCRATCH3;
    call(PPC_ADDR_PSMTXCopy);
    get_mtx(SCRATCH3, m);
    CHECK(memcmp(m, a, sizeof(m)) == 0, "PSMTXCopy");

    ctx.r[3] = SCRATCH; ctx.r[4] = SCRATCH3;
    call(PPC_ADDR_PSMTXTranspose);
    get_mtx(SCRATCH3, m);
    ok = 1;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            if (m[r * 4 + c] != a[c * 4 + r]) ok = 0;
    CHECK(ok && m[3] == 0 && m[7] == 0 && m[11] == 0, "PSMTXTranspose");
}

static void test_vec(void) {
    const float u[3] = {1.5f, -2.0f, 3.25f}, v[3] = {-0.5f, 4.0f, 2.0f};
    for (int i = 0; i < 3; i++) { stf32(SCRATCH + 4 * i, u[i]); stf32(SCRATCH2 + 4 * i, v[i]); }
    ctx.r[3] = SCRATCH; ctx.r[4] = SCRATCH2; ctx.r[5] = SCRATCH3;
    call(PPC_ADDR_PSVECAdd);
    int ok = 1;
    for (int i = 0; i < 3; i++) if (ldf32(SCRATCH3 + 4 * i) != u[i] + v[i]) ok = 0;
    CHECK(ok, "PSVECAdd");

    ctx.r[3] = SCRATCH; ctx.r[4] = SCRATCH2; ctx.r[5] = SCRATCH3;
    call(PPC_ADDR_PSVECCrossProduct);
    float c[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    ok = 1;
    for (int i = 0; i < 3; i++) if (fabsf(ldf32(SCRATCH3 + 4 * i) - c[i]) > 1e-5f) ok = 0;
    CHECK(ok, "PSVECCrossProduct");

    ctx.r[3] = SCRATCH; ctx.r[4] = SCRATCH3;
    call(PPC_ADDR_PSVECNormalize);
    double len = 0, ul = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    ok = 1;
    for (int i = 0; i < 3; i++) {
        double x = ldf32(SCRATCH3 + 4 * i);
        len += x * x;
        if (fabs(x - u[i] / ul) > 1e-4) ok = 0;
    }
    CHECK(ok && fabs(len - 1.0) < 1e-4, "PSVECNormalize (len^2=%f)", len);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: %s <main.dol>\n", argv[0]);
        return 2;
    }
    if (!ppc_mem_init() || !ppc_load_dol(argv[1], NULL)) {
        printf("could not load %s\n", argv[1]);
        return 2;
    }
    ppc_ctx_init(&ctx);
    call(PPC_ADDR___init_registers);  // sets r1 (stack), r2 / r13 (small data bases)
    printf("r1=%08X r2=%08X r13=%08X\n", ctx.r[1], ctx.r[2], ctx.r[13]);

    test_mem();
    test_int64();
    test_mtx();
    test_vec();

    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
