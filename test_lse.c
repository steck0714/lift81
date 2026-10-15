/*
 * test_lse.c - LSE エミュレータの差分テスト (aarch64 Linux ユーザー空間)
 *
 *   qemu-aarch64 -cpu max        ./test_lse native   # 本物のLSEで実行 (期待値)
 *   qemu-aarch64 -cpu cortex-a57 ./test_lse emul     # LSE無し(v8.0) -> SIGILL -> lse_emulate
 *
 * 両方の出力 (エンコーディングごとのハッシュ) が一致すればOK。
 * 生成した命令を RWX バッファに置いて実行するので、全エンコードを網羅できる。
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include "lse_emul.h"

static int emul_mode;
static unsigned long hits;

/* ---------------- SIGILL -> lse_emulate ---------------- */
static void on_sigill(int sig, siginfo_t *si, void *uctx)
{
    (void)sig; (void)si;
    ucontext_t *uc = uctx;
    lse_ctx c;
    for (int i = 0; i < 31; i++) c.x[i] = uc->uc_mcontext.regs[i];
    c.sp = uc->uc_mcontext.sp;
    c.pc = uc->uc_mcontext.pc;

    uint32_t insn = *(volatile uint32_t *)c.pc;
    int r = lse_emulate(&c, insn);
    if (r != LSE_OK) {
        char m[80];
        int n = snprintf(m, sizeof m, "emul failed: insn=%08x rc=%d\n", insn, r);
        if (write(2, m, n) < 0) {}
        _exit(99);
    }
    for (int i = 0; i < 31; i++) uc->uc_mcontext.regs[i] = c.x[i];
    uc->uc_mcontext.sp = c.sp;
    uc->uc_mcontext.pc = c.pc;
    __atomic_fetch_add(&hits, 1, __ATOMIC_RELAXED);
}

/* ---------------- 命令実行スタブ ---------------- */
/* regs[0]=x0 regs[1]=x2 regs[2]=x3 regs[3]=x4 regs[4]=x5 ; code は [insn][ret] */
extern void exec_stub(uint64_t *regs, void *code);
extern void exec_stub_sp(uint64_t *regs, void *code);   /* regs[0] = 32byteバッファのアドレス。命令は SP を Rn に使う */
__asm__(
    ".text\n"
    ".globl exec_stub\n"
    "exec_stub:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  mov x9, x0\n"
    "  mov x10, x1\n"
    "  ldr x0, [x9, #0]\n"
    "  ldr x2, [x9, #8]\n"
    "  ldr x3, [x9, #16]\n"
    "  ldr x4, [x9, #24]\n"
    "  ldr x5, [x9, #32]\n"
    "  blr x10\n"
    "  str x2, [x9, #8]\n"
    "  str x3, [x9, #16]\n"
    "  str x4, [x9, #24]\n"
    "  str x5, [x9, #32]\n"
    "  ldp x29, x30, [sp], #16\n"
    "  ret\n"
    ".globl exec_stub_sp\n"
    "exec_stub_sp:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  mov x9, x0\n"
    "  mov x10, x1\n"
    "  ldr x11, [x9, #0]\n"
    "  sub sp, sp, #32\n"
    "  ldp x12, x13, [x11]\n"
    "  stp x12, x13, [sp]\n"
    "  ldp x12, x13, [x11, #16]\n"
    "  stp x12, x13, [sp, #16]\n"
    "  ldr x2, [x9, #8]\n"
    "  ldr x3, [x9, #16]\n"
    "  ldr x4, [x9, #24]\n"
    "  ldr x5, [x9, #32]\n"
    "  blr x10\n"
    "  str x2, [x9, #8]\n"
    "  str x3, [x9, #16]\n"
    "  str x4, [x9, #24]\n"
    "  str x5, [x9, #32]\n"
    "  ldp x12, x13, [sp]\n"
    "  stp x12, x13, [x11]\n"
    "  ldp x12, x13, [sp, #16]\n"
    "  stp x12, x13, [x11, #16]\n"
    "  add sp, sp, #32\n"
    "  ldp x29, x30, [sp], #16\n"
    "  ret\n");

static uint32_t *code;

static uint64_t fnv(uint64_t h, uint64_t v)
{
    for (int i = 0; i < 8; i++) { h ^= (v >> (8 * i)) & 0xff; h *= 0x100000001b3ULL; }
    return h;
}

static const uint64_t vals[] = {
    0x0000000000000000ULL, 0x0000000000000001ULL, 0x000000000000007fULL,
    0x0000000000000080ULL, 0x00000000000000ffULL, 0x0000000000007fffULL,
    0x0000000000008000ULL, 0x000000007fffffffULL, 0x0000000080000000ULL,
    0x00000000ffffffffULL, 0x7fffffffffffffffULL, 0x8000000000000000ULL,
    0xffffffffffffffffULL, 0x0123456789abcdefULL, 0xfedcba9876543210ULL,
};
#define NVALS (sizeof vals / sizeof vals[0])

/* 1命令を1ケース実行し、結果(レジスタ+メモリ)をハッシュに畳む */
static uint64_t run_case(uint64_t h, uint32_t insn, int use_sp, uint64_t m0, uint64_t m1,
                         uint64_t x2, uint64_t x3, uint64_t x4, uint64_t x5)
{
    static uint64_t buf[4] __attribute__((aligned(16)));
    buf[0] = m0; buf[1] = m1;
    buf[2] = 0xC0FFEE0011223344ULL; buf[3] = 0x5566778899AABBCCULL;   /* カナリア */

    code[0] = insn;
    code[1] = 0xd65f03c0;                                             /* ret */
    __builtin___clear_cache((char *)code, (char *)(code + 2));

    uint64_t regs[5] = { (uint64_t)(uintptr_t)buf, x2, x3, x4, x5 };
    if (use_sp) exec_stub_sp(regs, code);
    else        exec_stub(regs, code);

    for (int i = 1; i < 5; i++) h = fnv(h, regs[i]);
    for (int i = 0; i < 4; i++) h = fnv(h, buf[i]);
    return h;
}

static const char *amo_names[9] = { "ldadd","ldclr","ldeor","ldset","ldsmax","ldsmin","ldumax","ldumin","swp" };
static const char sfx[4] = { 'b', 'h', 'w', 'x' };
static int verbose;

static uint64_t g_total = 14695981039346656037ULL;
static int g_groups;

static void group_done(const char *name, uint64_t h, int ncase)
{
    printf("%-22s cases=%-5d hash=%016llx\n", name, ncase, (unsigned long long)h);
    g_total = fnv(g_total, h);
    g_groups++;
}

static void test_amo(void)
{
    for (int op = 0; op < 9; op++)
    for (int sz = 0; sz < 4; sz++)
    for (int a = 0; a < 2; a++)
    for (int l = 0; l < 2; l++)
    for (int var = 0; var < 4; var++) {
        /* var 0: Rn=x0 Rs=x2 Rt=x4 / 1: Rt=XZR (ST*) / 2: Rs=XZR / 3: Rn=SP */
        unsigned rn = 0, rs = 2, rt = 4;
        if (var == 1) rt = 31;
        if (var == 2) rs = 31;
        if (var == 3) rn = 31;
        unsigned o3 = (op == 8), opc = (op == 8) ? 0 : op;
        uint32_t insn = ((uint32_t)sz << 30) | 0x38200000u | (a << 23) | (l << 22) |
                        (rs << 16) | (o3 << 15) | (opc << 12) | (rn << 5) | rt;
        uint64_t h = 14695981039346656037ULL;
        int n = 0;
        for (unsigned i = 0; i < NVALS; i++)
        for (unsigned j = 0; j < NVALS; j++) {
            uint64_t m = vals[i], rsv = vals[j];
            h = run_case(h, insn, var == 3, m, ~m, rsv, 0x1111, 0xDEADBEEFCAFEF00DULL, 0x2222);
            n++;
        }
        char name[48];
        snprintf(name, sizeof name, "%s%s%s%c%s", amo_names[op], a ? "a" : "", l ? "l" : "",
                 sfx[sz], var == 1 ? " rt=xzr" : var == 2 ? " rs=xzr" : var == 3 ? " rn=sp" : "");
        group_done(name, h, n);
    }
}

static void test_cas(void)
{
    for (int sz = 0; sz < 4; sz++)
    for (int a = 0; a < 2; a++)
    for (int l = 0; l < 2; l++)
    for (int var = 0; var < 3; var++) {
        /* var 0: 通常 / 1: Rt=XZR / 2: Rn=SP */
        unsigned rn = var == 2 ? 31 : 0, rs = 2, rt = var == 1 ? 31 : 4;
        uint32_t insn = ((uint32_t)sz << 30) | 0x08A07C00u | (a << 22) | (l << 15) |
                        (rs << 16) | (rn << 5) | rt;
        uint64_t h = 14695981039346656037ULL;
        int n = 0;
        for (unsigned i = 0; i < NVALS; i++) {
            uint64_t m = vals[i];
            /* 期待値の候補: 一致 / 上位だけ違う(切り詰めで一致するか) / 下位が違う / 無関係 */
            uint64_t cand[] = { m, m ^ 0xFF00ULL, m ^ 0xFFFFFFFF00000000ULL, m ^ 1,
                                vals[(i + 3) % NVALS], ~m };
            for (unsigned k = 0; k < sizeof cand / sizeof cand[0]; k++) {
                h = run_case(h, insn, var == 2, m, ~m, cand[k], 0x1111,
                             0xDEADBEEFCAFEF00DULL, 0x2222);
                n++;
            }
        }
        char name[48];
        snprintf(name, sizeof name, "cas%s%s%c%s", a ? "a" : "", l ? "l" : "", sfx[sz],
                 var == 1 ? " rt=xzr" : var == 2 ? " rn=sp" : "");
        group_done(name, h, n);
    }
}

static void test_casp(void)
{
    for (int sz = 0; sz < 2; sz++)
    for (int a = 0; a < 2; a++)
    for (int l = 0; l < 2; l++) {
        /* Rs=x2(x2,x3) Rt=x4(x4,x5) Rn=x0 */
        uint32_t insn = ((uint32_t)sz << 30) | 0x08207C00u | (a << 22) | (l << 15) |
                        (2u << 16) | (0u << 5) | 4u;
        uint64_t h = 14695981039346656037ULL;
        int n = 0;
        for (unsigned i = 0; i < NVALS; i++)
        for (unsigned j = 0; j < NVALS; j += 2) {
            uint64_t m0 = vals[i], m1 = vals[j];
            uint64_t f0, f1;                         /* メモリ上のペアの要素 */
            if (sz == 0) { f0 = m0 & 0xffffffffULL; f1 = m0 >> 32; }
            else         { f0 = m0; f1 = m1; }
            for (int match = 0; match < 3; match++) {
                /* 0: 両方一致 1: 先頭だけ一致 2: 2番目だけ一致 */
                uint64_t e0 = (match == 2) ? ~f0 : f0;
                uint64_t e1 = (match == 1) ? ~f1 : f1;
                if (sz == 0) { e0 |= 0xABCD000000000000ULL; e1 |= 0x1234000000000000ULL; } /* 上位ゴミ */
                h = run_case(h, insn, 0, m0, m1, e0, e1, 0xAAAAAAAAAAAAAAA1ULL, 0xBBBBBBBBBBBBBBB2ULL);
                n++;
            }
        }
        char name[48];
        snprintf(name, sizeof name, "casp%s%s%c", a ? "a" : "", l ? "l" : "", sz ? 'x' : 'w');
        group_done(name, h, n);
    }
}


/* ---------------- エラー経路 (トラップ不要: 関数を直接呼ぶ) ---------------- */
static int test_errors(void)
{
    static uint64_t buf[4] __attribute__((aligned(16))) = { 1, 2, 3, 4 };
    lse_ctx c;
    int bad = 0;
#define CHK(cond, msg) do { if (!(cond)) { printf("errpath FAIL: %s\n", msg); bad++; } } while (0)

    memset(&c, 0, sizeof c);
    c.pc = 0x1000;

    /* 非自然アライン -> LSE_ALIGN。ctx/メモリは不変 */
    uint32_t ldaddal_w = (2u << 30) | 0x38200000u | (1 << 23) | (1 << 22) | (2u << 16) | (0u << 5) | 4u;
    c.x[0] = (uint64_t)(uintptr_t)buf + 1;
    CHK(lse_emulate(&c, ldaddal_w) == LSE_ALIGN, "misaligned ldaddal w -> ALIGN");
    CHK(c.pc == 0x1000 && buf[0] == 1, "ALIGN leaves ctx/mem untouched");

    /* CASP: Rs / Rt が奇数 -> LSE_UNDEF */
    uint32_t casp_odd_rs = 0x08207C00u | (3u << 16) | (0u << 5) | 4u;
    uint32_t casp_odd_rt = 0x08207C00u | (2u << 16) | (0u << 5) | 5u;
    c.x[0] = (uint64_t)(uintptr_t)buf;
    CHK(lse_emulate(&c, casp_odd_rs) == LSE_UNDEF, "casp odd Rs -> UNDEF");
    CHK(lse_emulate(&c, casp_odd_rt) == LSE_UNDEF, "casp odd Rt -> UNDEF");

    /* CASP 64bit は 16byte アラインが必要 */
    uint32_t casp_x = (1u << 30) | 0x08207C00u | (2u << 16) | (0u << 5) | 4u;
    c.x[0] = (uint64_t)(uintptr_t)buf + 8;
    CHK(lse_emulate(&c, casp_x) == LSE_ALIGN, "casp x misaligned(8) -> ALIGN");

    /* 対象外 -> LSE_NOT_LSE: LDAPR(FEAT_LRCPC) / NOP / LDAXR / ADD / LDR */
    uint32_t ldapr_x = (3u << 30) | 0x38200000u | (1 << 23) | (0x1fu << 16) | (1 << 15) | (4u << 12) | 4u;
    uint32_t others[] = { ldapr_x, 0xd503201fu, 0xc85ffc01u, 0x8b020021u, 0xf9400001u, 0x00000000u };
    for (unsigned i = 0; i < sizeof others / sizeof others[0]; i++)
        CHK(lse_emulate(&c, others[i]) == LSE_NOT_LSE, "non-LSE -> NOT_LSE");

    printf("errpaths: %d failures\n", bad);
    return bad;
}

/* ---------------- 並行テスト: 複数スレッドで本当にアトミックか ---------------- */
#define NTHREADS 4
#define NITER    5000
static uint64_t counter __attribute__((aligned(16)));
static uint64_t casctr  __attribute__((aligned(16)));

__attribute__((target("+lse")))
static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < NITER; i++) {
        uint64_t one = 1, out;
        __asm__ volatile("ldaddal %x2, %x0, [%1]" : "=&r"(out) : "r"(&counter), "r"(one) : "memory");
        __asm__ volatile("stadd %x1, [%0]" :: "r"(&counter), "r"(one) : "memory");
        /* CAS ループ */
        for (;;) {
            uint64_t cur = __atomic_load_n(&casctr, __ATOMIC_RELAXED);
            uint64_t expected = cur;
            __asm__ volatile("casal %x0, %x2, [%1]" : "+r"(expected) : "r"(&casctr), "r"(cur + 1) : "memory");
            if (expected == cur) break;
        }
    }
    return NULL;
}

static int test_threads(void)
{
    pthread_t th[NTHREADS];
    counter = 0; casctr = 0;
    for (int i = 0; i < NTHREADS; i++) pthread_create(&th[i], NULL, worker, NULL);
    for (int i = 0; i < NTHREADS; i++) pthread_join(th[i], NULL);
    uint64_t want_c = (uint64_t)NTHREADS * NITER * 2, want_k = (uint64_t)NTHREADS * NITER;
    printf("threads: ldaddal counter=%llu (want %llu)  casal counter=%llu (want %llu)\n",
           (unsigned long long)counter, (unsigned long long)want_c,
           (unsigned long long)casctr, (unsigned long long)want_k);
    return !(counter == want_c && casctr == want_k);
}

int main(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "native") && strcmp(argv[1], "emul"))) {
        fprintf(stderr, "usage: %s native|emul [-v]\n", argv[0]);
        return 2;
    }
    emul_mode = !strcmp(argv[1], "emul");
    verbose = argc > 2;
    (void)verbose;

    printf("mode=%s HWCAP_ATOMICS=%d\n", argv[1], !!(getauxval(AT_HWCAP) & (1 << 8)));

    if (emul_mode) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_sigill;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGILL, &sa, NULL);
    }

    code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) { perror("mmap"); return 1; }

    test_amo();
    test_cas();
    test_casp();
    int bad = test_threads();
    bad += test_errors();

    printf("TOTAL groups=%d hash=%016llx\n", g_groups, (unsigned long long)g_total);
    fprintf(stderr, "trap hits=%lu\n", hits);
    return bad;
}
