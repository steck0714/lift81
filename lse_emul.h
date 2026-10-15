/*
 * lse_emul.h - ARMv8.1 LSE atomics -> ARMv8.0 LL/SC emulation core
 *
 * 対象: ARMv8.0 (例: Tegra X1 / T210 の Cortex-A57/A53) 上で、
 *       LSE (FEAT_LSE) 命令を「未定義命令例外」から救済する。
 *
 * このコアは freestanding (libc不要) で、トラップフレームの
 * x0-x30 / SP / PC だけを触る。EL1 の undef ハンドラ、EL2 の
 * ハイパーバイザ、Linux の SIGILL ハンドラなど、どこからでも呼べる。
 *
 * 注意: 変換の向きは「LSE命令 -> LDXR/STXR ループ」。
 *       (LDREX/STREX はAArch32の名前で、AArch64では LDXR/STXR)
 */
#ifndef LSE_EMUL_H
#define LSE_EMUL_H

#include <stdint.h>

typedef struct {
    uint64_t x[31];   /* x0..x30 (XZRは持たない) */
    uint64_t sp;      /* 命令の Rn==31 が指す SP (EL0ならSP_EL0 等、呼び出し側が選ぶ) */
    uint64_t pc;      /* 例外を起こした命令のアドレス。成功時に +4 される */
} lse_ctx;

enum {
    LSE_OK        = 0, /* エミュレート成功。ctx->pc は次の命令に進んでいる */
    LSE_NOT_LSE   = 1, /* LSE命令ではない。通常のUndefined扱いにすること */
    LSE_UNDEF     = 2, /* LSEの未割り当てエンコード (CASPの奇数レジスタ等)。Undefinedを注入 */
    LSE_ALIGN     = 3  /* アドレスが自然アラインでない。実機同様 Alignment fault を注入 */
};

/*
 * insn: 例外を起こした命令語 (32bit)。
 * 戻り値: LSE_*。ctx は LSE_OK のときだけ更新される。
 *
 * 対応: LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/LDUMAX/LDUMIN/SWP
 *       (B/H/W/X, A/L/AL 付き, ST* エイリアス含む), CAS(B/H/W/X, A/L/AL), CASP(W/X)
 * 非対応: LDAPR(FEAT_LRCPC), LD64B系(FEAT_LS64), 128bit LSE128 -> LSE_NOT_LSE
 */
int lse_emulate(lse_ctx *c, uint32_t insn);

#endif
