/*
 * lse_emul.c - LSE (ARMv8.1 atomics) を ARMv8.0 の LL/SC で代替する
 *
 * エンコードの参照元:
 *   QEMU target/arm/tcg/a64.decode (@atomic / CAS / CASP)
 * LL/SCループの形の参照元:
 *   Linux arch/arm64/include/asm/atomic_ll_sc.h
 *
 * 設計メモ
 *  - 順序付けは常に LDAXR/STLXR (acquire+release) を使う。
 *    A/L ビットが付いていない緩い命令に対して強い順序を与えるのは
 *    アーキテクチャ上許される (見える挙動が強くなるだけ)。
 *  - このファイル自体は ARMv8.0 命令だけでできていること
 *    (Makefile の `make check` で objdump して LSE 命令が無いことを確認)。
 *  - ループ中はメモリアクセスを挟まない: 演算は全部インラインasm内で行う。
 */
#include "lse_emul.h"

/* ---------------------------------------------------------------- */
/* RMW (LDADD 系 + SWP): old を返し、メモリに op(old, v) を書く       */
/* %0=old %1=new %2=status %3=ptr %4=operand                         */
/* ---------------------------------------------------------------- */

#define OP_ADD(R, SX, ZX)  "add %" R "1, %" R "0, %" R "4\n"
#define OP_CLR(R, SX, ZX)  "bic %" R "1, %" R "0, %" R "4\n"
#define OP_EOR(R, SX, ZX)  "eor %" R "1, %" R "0, %" R "4\n"
#define OP_SET(R, SX, ZX)  "orr %" R "1, %" R "0, %" R "4\n"
#define OP_SWP(R, SX, ZX)  "mov %" R "1, %" R "4\n"
/* 符号付き: old を符号拡張して x1 に置き、64bitで比較 (operandは事前に符号拡張済み) */
#define OP_SMAX(R, SX, ZX) SX "cmp %x1, %x4\n" "csel %x1, %x1, %x4, gt\n"
#define OP_SMIN(R, SX, ZX) SX "cmp %x1, %x4\n" "csel %x1, %x1, %x4, lt\n"
/* 符号なし: old をゼロ拡張して x1 に置き、64bitで比較 (operandは事前にゼロ拡張済み) */
#define OP_UMAX(R, SX, ZX) ZX "cmp %x1, %x4\n" "csel %x1, %x1, %x4, hi\n"
#define OP_UMIN(R, SX, ZX) ZX "cmp %x1, %x4\n" "csel %x1, %x1, %x4, lo\n"

#define RMW_FN(OPN, SZN, SFX, R, SX, ZX, OPM)                              \
static uint64_t rmw_##OPN##_##SZN(void *p, uint64_t v)                     \
{                                                                          \
    uint64_t old, nv;                                                      \
    uint32_t st;                                                           \
    __asm__ volatile(                                                      \
        "1: ldaxr" SFX " %" R "0, [%3]\n"                                  \
        OPM(R, SX, ZX)                                                     \
        "   stlxr" SFX " %w2, %" R "1, [%3]\n"                             \
        "   cbnz  %w2, 1b\n"                                               \
        : "=&r"(old), "=&r"(nv), "=&r"(st)                                 \
        : "r"(p), "r"(v)                                                   \
        : "cc", "memory");                                                 \
    return old;                                                            \
}

#define RMW_ALL_SIZES(OPN, OPM)                                                      \
    RMW_FN(OPN, b, "b", "w", "sxtb %x1, %w0\n", "uxtb %w1, %w0\n", OPM)              \
    RMW_FN(OPN, h, "h", "w", "sxth %x1, %w0\n", "uxth %w1, %w0\n", OPM)              \
    RMW_FN(OPN, w, "",  "w", "sxtw %x1, %w0\n", "mov %w1, %w0\n",  OPM)              \
    RMW_FN(OPN, x, "",  "x", "mov %x1, %x0\n",  "mov %x1, %x0\n",  OPM)

RMW_ALL_SIZES(add,  OP_ADD)
RMW_ALL_SIZES(clr,  OP_CLR)
RMW_ALL_SIZES(eor,  OP_EOR)
RMW_ALL_SIZES(set,  OP_SET)
RMW_ALL_SIZES(smax, OP_SMAX)
RMW_ALL_SIZES(smin, OP_SMIN)
RMW_ALL_SIZES(umax, OP_UMAX)
RMW_ALL_SIZES(umin, OP_UMIN)
RMW_ALL_SIZES(swp,  OP_SWP)

typedef uint64_t (*rmw_fn)(void *, uint64_t);

#define ROW(OPN) { rmw_##OPN##_b, rmw_##OPN##_h, rmw_##OPN##_w, rmw_##OPN##_x }
static const rmw_fn rmw_tab[9][4] = {
    ROW(add), ROW(clr), ROW(eor), ROW(set),
    ROW(smax), ROW(smin), ROW(umax), ROW(umin),
    ROW(swp),
};

/* ---------------------------------------------------------------- */
/* CAS: old を返す。old==expected(サイズに切り詰め) のときだけ書く     */
/* %0=old %1=status %2=ptr %3=expected %4=desired                    */
/* ---------------------------------------------------------------- */
#define CAS_FN(SZN, SFX, R, CMP)                                           \
static uint64_t cas_##SZN(void *p, uint64_t e, uint64_t d)                 \
{                                                                          \
    uint64_t old;                                                          \
    uint32_t st;                                                           \
    __asm__ volatile(                                                      \
        "1: ldaxr" SFX " %" R "0, [%2]\n"                                  \
        CMP                                                                \
        "   b.ne  2f\n"                                                    \
        "   stlxr" SFX " %w1, %" R "4, [%2]\n"                             \
        "   cbnz  %w1, 1b\n"                                               \
        "2: clrex\n"                                                       \
        : "=&r"(old), "=&r"(st)                                            \
        : "r"(p), "r"(e), "r"(d)                                           \
        : "cc", "memory");                                                 \
    return old;                                                            \
}
CAS_FN(b, "b", "w", "cmp %w0, %w3, uxtb\n")
CAS_FN(h, "h", "w", "cmp %w0, %w3, uxth\n")
CAS_FN(w, "",  "w", "cmp %w0, %w3\n")
CAS_FN(x, "",  "x", "cmp %x0, %x3\n")

typedef uint64_t (*cas_fn)(void *, uint64_t, uint64_t);
static const cas_fn cas_tab[4] = { cas_b, cas_h, cas_w, cas_x };

/* ---------------------------------------------------------------- */
/* CASP: ペア (低アドレス側が先頭) の比較交換                         */
/* %0,%1=old %2=status %3=ptr %4,%5=expected %6,%7=desired           */
/* ---------------------------------------------------------------- */
#define CASP_FN(SZN, R)                                                    \
static void casp_##SZN(void *p, uint64_t e0, uint64_t e1,                  \
                       uint64_t d0, uint64_t d1,                           \
                       uint64_t *o0, uint64_t *o1)                         \
{                                                                          \
    uint64_t a, b;                                                         \
    uint32_t st;                                                           \
    __asm__ volatile(                                                      \
        "1: ldaxp %" R "0, %" R "1, [%3]\n"                                \
        "   cmp   %" R "0, %" R "4\n"                                      \
        "   ccmp  %" R "1, %" R "5, #0, eq\n"                              \
        "   b.ne  2f\n"                                                    \
        "   stlxp %w2, %" R "6, %" R "7, [%3]\n"                           \
        "   cbnz  %w2, 1b\n"                                               \
        "2: clrex\n"                                                       \
        : "=&r"(a), "=&r"(b), "=&r"(st)                                    \
        : "r"(p), "r"(e0), "r"(e1), "r"(d0), "r"(d1)                       \
        : "cc", "memory");                                                 \
    *o0 = a;                                                               \
    *o1 = b;                                                               \
}
CASP_FN(w, "w")
CASP_FN(x, "x")

/* ---------------------------------------------------------------- */
/* レジスタアクセス (31 = XZR / 書き込み破棄)。アドレスの Rn=31 は SP  */
/* ---------------------------------------------------------------- */
static inline uint64_t rz(const lse_ctx *c, unsigned r) { return r == 31 ? 0 : c->x[r]; }
static inline void wz(lse_ctx *c, unsigned r, uint64_t v) { if (r != 31) c->x[r] = v; }
static inline uint64_t raddr(const lse_ctx *c, unsigned r) { return r == 31 ? c->sp : c->x[r]; }

static inline uint64_t zext(uint64_t v, unsigned sz)
{
    return sz == 3 ? v : (v & ((1ULL << (8u << sz)) - 1));
}
static inline uint64_t sext(uint64_t v, unsigned sz)
{
    if (sz == 3) return v;
    unsigned sh = 64 - (8u << sz);
    return (uint64_t)((int64_t)(v << sh) >> sh);
}

/* ---------------------------------------------------------------- */
/* デコード                                                           */
/* ---------------------------------------------------------------- */

/* LDADD..LDUMIN / SWP : size 111 0 00 A R 1 Rs o3 opc 00 Rn Rt */
static int do_amo(lse_ctx *c, uint32_t insn)
{
    unsigned sz  = insn >> 30;
    unsigned rs  = (insn >> 16) & 31;
    unsigned o3  = (insn >> 15) & 1;
    unsigned opc = (insn >> 12) & 7;
    unsigned rn  = (insn >> 5) & 31;
    unsigned rt  = insn & 31;
    unsigned op;

    if (o3 == 0)
        op = opc;                 /* 0..7: add clr eor set smax smin umax umin */
    else if (opc == 0)
        op = 8;                   /* SWP */
    else
        return LSE_NOT_LSE;       /* LDAPR(LRCPC), LD64B(LS64) 等は対象外 */

    uint64_t addr = raddr(c, rn);
    if (addr & ((1ULL << sz) - 1))
        return LSE_ALIGN;

    uint64_t v = rz(c, rs);
    if (op == 4 || op == 5)       v = sext(v, sz);
    else if (op == 6 || op == 7)  v = zext(v, sz);

    uint64_t old = rmw_tab[op][sz]((void *)(uintptr_t)addr, v);

    wz(c, rt, old);               /* ST* エイリアス(Rt=XZR)は自然に破棄される */
    c->pc += 4;
    return LSE_OK;
}

/* CAS / CASA / CASAL / CASL : size 001000 1 L 1 Rs o0 11111 Rn Rt */
static int do_cas(lse_ctx *c, uint32_t insn)
{
    unsigned sz = insn >> 30;
    unsigned rs = (insn >> 16) & 31;
    unsigned rn = (insn >> 5) & 31;
    unsigned rt = insn & 31;

    uint64_t addr = raddr(c, rn);
    if (addr & ((1ULL << sz) - 1))
        return LSE_ALIGN;

    uint64_t old = cas_tab[sz]((void *)(uintptr_t)addr, rz(c, rs), rz(c, rt));

    wz(c, rs, old);               /* Rs には常に読んだ値(ゼロ拡張)が入る */
    c->pc += 4;
    return LSE_OK;
}

/* CASP / CASPA / CASPAL / CASPL : 0 sz 001000 0 L 1 Rs o0 11111 Rn Rt */
static int do_casp(lse_ctx *c, uint32_t insn)
{
    unsigned sz = (insn >> 30) & 1;   /* 0: 32bit x2, 1: 64bit x2 */
    unsigned rs = (insn >> 16) & 31;
    unsigned rn = (insn >> 5) & 31;
    unsigned rt = insn & 31;

    if ((rs & 1) || (rt & 1))
        return LSE_UNDEF;             /* アーキテクチャ上 UNDEFINED */

    uint64_t addr = raddr(c, rn);
    if (addr & ((2ULL << (sz + 2)) - 1))   /* ペア全体で自然アライン (8 / 16 byte) */
        return LSE_ALIGN;

    uint64_t e0 = rz(c, rs), e1 = rz(c, rs + 1);
    uint64_t d0 = rz(c, rt), d1 = rz(c, rt + 1);
    uint64_t o0, o1;

    if (sz == 0)
        casp_w((void *)(uintptr_t)addr, e0, e1, d0, d1, &o0, &o1);
    else
        casp_x((void *)(uintptr_t)addr, e0, e1, d0, d1, &o0, &o1);

    wz(c, rs, o0);
    wz(c, rs + 1, o1);
    c->pc += 4;
    return LSE_OK;
}

int lse_emulate(lse_ctx *c, uint32_t insn)
{
    if ((insn & 0x3F200C00u) == 0x38200000u) return do_amo(c, insn);
    if ((insn & 0x3FA07C00u) == 0x08A07C00u) return do_cas(c, insn);
    if ((insn & 0xBFA07C00u) == 0x08207C00u) return do_casp(c, insn);
    return LSE_NOT_LSE;
}
