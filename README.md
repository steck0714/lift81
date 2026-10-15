# lift81

ARMv8.0 の CPU (例: Tegra X1 / **T210** = Cortex-A57/A53) で、LSE 命令
(`CAS`/`CASP`/`SWP`/`LDADD`/`LDCLR`/`LDEOR`/`LDSET`/`LDSMAX`/`LDSMIN`/`LDUMAX`/`LDUMIN`/`ST*`)
を **LDAXR/STLXR ループに変換して実行**するエミュレーション・コア。
Win11 24H2 以降が LSE を要求する問題の、最初の部品。

> 変換の向きは **LSE → LL/SC**。(`LDREX/STREX` は AArch32 の名前。AArch64 は `LDXR/STXR`。)

## ファイル

| ファイル | 内容 |
|---|---|
| `lse_emul.h / .c` | コア。freestanding、v8.0 命令のみ、FP/NEON 不使用。`lse_emulate(ctx, insn)` |
| `test_lse.c` | 差分テスト。生成した全エンコードを「実LSE」と「エミュレータ」で実行して比較 |
| `Makefile` | `make test` で一括検証 |

## 使い方 (コア)

```c
lse_ctx c = { .x = {...}, .sp = sp, .pc = elr };   // トラップフレームからコピー
int r = lse_emulate(&c, insn);                       // insn = *(u32*)elr
// LSE_OK       : c を書き戻して ERET (pc は +4 済み)
// LSE_NOT_LSE  : 普通の Undefined として扱う
// LSE_UNDEF    : Undefined を注入 (CASP の奇数レジスタ等)
// LSE_ALIGN    : Alignment fault を注入 (LSE2 の無い CPU は実機も fault)
```

## 検証状況 (qemu-user, aarch64)

`make test` は同じバイナリを 2 通りで走らせ、全結果のハッシュを比較する。

- `qemu-aarch64 -cpu max` : 本物の LSE で実行 → 期待値
- `qemu-aarch64 -cpu cortex-a57` : LSE 無し (v8.0)。SIGILL → `lse_emulate` → 続行

結果: **632 グループ / 約 20 万トラップで完全一致**。

- 全エンコード網羅: 9 演算 × 4 サイズ × A/L × {通常, Rt=XZR(ST*), Rs=XZR, Rn=SP}、CAS 4 サイズ × A/L、CASP W/X
- 境界値 (符号境界、サイズ切り詰め、上位ゴミ、CAS の「上位だけ違う」ケース)
- 4 スレッド並行で `LDADDAL` / `CASAL` のカウンタが厳密に一致 (他コアに対してアトミック)
- エラー経路 (ALIGN / UNDEF / NOT_LSE) の単体テスト
- コア自体に LSE 命令が混入していないことを `objdump` で確認 (`make check`)
- 変異テスト: エミュレータをわざと壊すと差分が出る (9〜65 グループ) ことを確認

**未検証**: 実機 T210 での動作、EL1/EL2 での組み込み、性能。qemu は A57 の
実際のメモリモデル/排他モニタ挙動を完全には再現しない。

## 設計上の判断

- **順序付けは常に acquire+release (LDAXR/STLXR)**。A/L ビットなしの緩い命令に
  強い順序を与えるのは許される (見える挙動が強くなるだけ)。
- ループ内にメモリアクセスを挟まない (演算は全てインライン asm)。排他モニタを壊さないため。
- LSE は前進保証があるが LL/SC は無い。極端な競合でライブロックし得る (短いループなので実用上は稀)。
- 対象外: `LDAPR` (FEAT_LRCPC)、`LD64B` 系 (FEAT_LS64)、LSE128 → `LSE_NOT_LSE`。

## 組み込み (T210 + Windows) — winx + 実際のT210用BL31 (ATF) を調べて設計

T210 で edk2 を実用的に使える現状唯一の系統として [steck0714/winx](https://github.com/steck0714/winx)
(imbushuo/NintendoSwitchPkg 派生, EDK2 202402) を読んだ。さらに、winx が使う `bl31.bin` の実体である
T210 向け ARM Trusted Firmware (ATF) 本体 — upstream `ARM-software/arm-trusted-firmware` には
T210 (Tegra210) の SoC コードが無く(upstream は t186/t194 以降のみ)、switchroot/LineageOS 系で
使われているのは [CTCaer/switch-l4t-atf](https://github.com/CTCaer/switch-l4t-atf)
(ブランチ `switch-v5-atf2.6`、`plat/nvidia/tegra/soc/t210/`) — も読み、ブート連鎖の各段で
どの EL に入るか、PSCI がどの conduit (SMC/HVC) を使うかを具体的に裏取りした。
設計の実現可能性を左右する「EL2 を奪えるか」「どこで CPU_ON を横取りするか」はこれで多くが埋まった。

### ブート連鎖で確認できた事実 (ソース根拠つき)

| 段階 | 確認できたこと | 根拠 |
|---|---|---|
| hekate → BL31 | hekate が `bl33.bin` を `0xAA000000` にロードし、BL31 がそこへ `pc=0xAA000000` で分岐 | winx `Tools/Bl33/bl33_stub.S` コメント |
| BL31 → BL33(UEFI) | **UEFI は `SPSR_EL2T` で入る。** TF-A 共通コード (`psci_get_ns_ep_info`, `bl31_main.c`) 自体は EL2/EL1 どちらでも対応できるが、実際にどちらで渡すかは hekate が組み立てる `bl33_ep_info->spsr` 次第。T210 の BL31 はこの値を BL2 相当の呼び出し元 (hekate) からそのまま受け取るだけ (`tegra_bl31_setup.c`: `bl33_image_ep_info = *arg_from_bl2->bl33_ep_info`) | switch-l4t-atf `plat/nvidia/tegra/common/tegra_bl31_setup.c`、winx のコメント |
| PSCI の呼び出し方 (conduit) | T210 の `platform.mk` に `PSCI_USE_HVC` の定義が無い → **既定の SMC**。winx の ACPI FADT (`ARM_PSCI_COMPLIANT` のみ、HVC フラグ無し) と一致 | switch-l4t-atf `plat/nvidia/tegra/soc/t210/platform_t210.mk`、winx `AcpiTables/Fadt.aslc` |
| セカンダリコア起動 | T210 の `plat_secondary_setup()` はセキュアモニタのリセットベクタ (`TEGRA_SB_BASE`) を設定するだけで、CPU_ON 自体は TF-A 共通の PSCI 実装 (`psci_get_ns_ep_info`) 経由。渡す `entry_point_info_t.spsr` で **要求した EL にそのまま入る** (EL2 を要求すれば `SCR_EL3.HCE` が立って EL2 へ) | switch-l4t-atf `plat/nvidia/tegra/soc/t210/plat_secondary.c`、`lib/el3_runtime/aarch64/context_mgmt.c` (`cm_set_next_eret_context` 周辺の `SCR_HCE_BIT` 判定) |
| EL2 レジスタの保存 | `CTX_INCLUDE_EL2_REGS` は RME (ARMv8.6) 向けの時だけ `1`。Cortex-A57 (v8.0) のビルドでは既定 `0` = **TF-A は EL2 レジスタの保存/復元をしない**。コア電源断を伴わない WFI 程度のアイドルなら EL2 の状態はハードウェアレジスタにそのまま残るはずだが、コアリセットを伴う深い電源状態からの復帰では EL2 の再初期化が要る (未検証) | switch-l4t-atf `Makefile` |

これで分かったのは、**「EL2 を奪う」こと自体は、BL33 (UEFI) が実際に EL2T で入ってきている以上、追加のトラップや細工なしに成立している**ということ。
winx の UEFI 自身は「EL1 に降りて UEFI を動かす」実装 (`ArmPlatformPeiBootAction` で `SPSel,#1` のみ、EL を見て VBAR_EL1 を張る分岐は見当たらず他機種の例と混ざって要再確認) になっており、現状 EL2 は「一瞬だけ存在して即 EL1 に降りる」フェーズになっていると見られる (ここは実コードの `PrePi`/`ArmPlatformPkg` 側の汎用処理なので、どこで EL1 に降ろしているか要追跡)。**訂正**: 当初「winx 独自の EL1 降下処理があるはず」と書いたが、実際には winx リポジトリにも
upstream の汎用 `ArmPlatformPkg/PrePi` (`ModuleEntryPoint.S`, `PrePi.c`, `MainMPCore.c`,
edk2-stable202402) にも、**EL2→EL1 を明示的に降ろす処理が見当たらない**。
`ArmPlatformPeiBootAction` (winx 実装) は `SPSel,#1` を設定するだけで EL には触れない。
つまり winx の UEFI は、BL33 の入口 (`SPSR_EL2T`) のまま **EL2 に居続けている可能性がある**
(実機で `CurrentEL` を読んで確認していないため断定はできない)。

もしそうなら、案Aはむしろ単純になる: 「EL2→EL1 遷移を乗っ取る」のではなく、
**UEFI が元々 EL2 に居る間に、常駐 EL2 コードを `ArmPlatformPeiBootAction` 相当の
最初期フックとして割り込ませればよい**。EL1 への `ERET` は不要で、UEFI は今まで通り動き続ける。
一方で新しい疑問が生まれる: **Windows のブートローダ (`winload`) やカーネルは EL1 を前提にしている
はずで、どこかで EL2→EL1 の遷移が起きるはず**。Linux は `arch/arm64/kernel/head.S` の
`init_el2`/`el2_setup` でこれを自前 (カーネル側) で行い、EL2 に残って nVHE ホストとして動く設計になっている
(他 OS の実装としての参考情報)。Windows が同様に自前で降りるのか、UEFI 側に降下を期待しているのかは
実機でのトレース (どこで `CurrentEL` が変わるか) でしか確認できない。

### 他機種の実例: EL2 で SMC/HVC を捕まえて書き換えるのは実際に出荷されている

Windows on ARM 系の別機種 [WOA-Project/Lumia950XLPkg](https://github.com/WOA-Project/Lumia950XLPkg) に、
**まさに同じ構造の実装**がある (`PrePi/HvcPatch.h`, `PrePi/Pi.c`)。既存ファームウェア (Qualcomm 側の
EL2 常駐コード) のバイナリを固定オフセットで書き換え、`HCR_EL2.TSC` を立てて SMC を EL2 にトラップし、
下位 EL からの同期例外ハンドラ内で命令をデコードして HVC/SMC を判別、`ELR_EL2` を進めてから
`smc #0` で EL3 に通す、というもの。コメントによると「Windows 19041 以降で HVC ベースの PSCI 呼び出しが
壊れる」問題への対処で、**Windows の ARM64 ブートで EL2 のトラップ/書き換えが実際に機能することの実例**になる。
winx/T210 のケースは ATF のソースを自分たちでビルドできる分、バイナリパッチより素直に書ける。

### 案A (EL2 常駐ハイパーバイザ) の更新した設計

1. **UEFI が EL1 へ降りる直前 (前述の遷移点) に、常駐 EL2 コードを割り込ませる。**
   `HCR_EL2`, `VBAR_EL2` を設定し、Stage-2 を恒等写像で有効化してから、元のコードと同じ `SPSR_EL1T` で
   `ERET` して UEFI の続きに戻す。以降 UEFI も Windows も EL1 (これは winx の現状とも矛盾しない)。
2. **Stage-2 で Windows の EL1 例外ベクタ (`VBAR_EL1`) のページを実行不可 (XN)** にし、Undefined の
   フェッチで EL2 に落として `lse_emulate` する — ここは前回設計のまま。
3. **SMP (複数コア) は PSCI CPU_ON の SMC を `HCR_EL2.TSC=1` でトラップして割り込む。**
   conduit が SMC であることは確定した (上表)。OS が secondary を起こす `CPU_ON(target_cpu, entry, ctx)`
   を EL2 で捕まえ、`entry`/`ctx` を保存した上で、EL3 への実際の `CPU_ON` 要求は **entry を自前の
   EL2 トランポリンに、SPSR を EL2 に書き換えて**発行する。secondary はそのトランポリンで
   HCR_EL2/VBAR_EL2 を設定してから、保存しておいた本来の `entry` へ `SPSR_EL1T` で `ERET` する。
   Lumia950XLPkg の `El2TrapSmcHandler` (`ELR_EL2+=4` → `smc #0` → `eret`) とほぼ同じ構造。
4. CTX_INCLUDE_EL2_REGS が既定で 0 なので、**電源断を伴う深い C-state からの復帰で EL2 の状態が
   飛ぶ可能性がある**。SC7 (System Suspend) のような深い休止からの復帰パスでも 1. と同じ
   「EL1 に降りる直前にEL2を再設定」フックが効く経路かどうかは実機確認が必要。
5. ユーザーモード (EL0) の LSE もベクタ +0x400 側の XN トラップで同様に救える (前回設計のまま)。

### 実機で最初に確認すべきこと (UART 115200 8N1 / winx は右レールのコネクタ)

- UEFI が EL1 へ降りる実際の箇所 (`PrePi`/`ArmPlatformPkg` 側、`ArmPlatformPeiBootAction` 周辺) をソースレベルで特定する。
- そこに最小の EL2 スタブ (何もせず EL1 へ `ERET` するだけ) を挟んでも winx が従来通り起動するか。
- OS の `CPU_ON` が本当に SMC で EL3 まで素通りしているか (EL2 で `HCR_EL2.TSC` を立てて実測)。
- Windows がカーネルをどの EL で起動するか、SC7 復帰時に EL2 の状態がどうなるか。
- 24H2 が LSE 以外に要求する機能 (PAN/RDM/VHE 等) が何か。

その他の障害: T210 には Windows 用 GPU ドライバ (WDDM) が無く、ACPI も CPU/GIC/timer/UART のみ。
LSE を解決しても SD/USB/表示/入力ドライバは未整備 (winx の README 参照)。
まずは Win10 ARM64 / Win11 23H2 以前で winx を起動させるほうが先。

## 参考にした GitHub 上のコード

- QEMU `target/arm/tcg/a64.decode` — `@atomic` / `CAS` / `CASP` のエンコード
- Linux `arch/arm64/include/asm/atomic_ll_sc.h` — LL/SC ループの形
- steck0714/winx — T210 向け EDK2 (hekate→BL31→BL33)。ブート連鎖・メモリマップ・ACPI 構成の把握に使用
- CTCaer/switch-l4t-atf (`switch-v5-atf2.6`) — T210 向け ATF (BL31) 本体。PSCI conduit / セカンダリ起動 / EL2 コンテキスト保存有無の確認に使用
- WOA-Project/Lumia950XLPkg (`PrePi/HvcPatch.h`, `Pi.c`) — 他機種での実例: EL2 で SMC/HVC をトラップしてPSCIを書き換える手法が実際にWindows ARM64ブートで機能している根拠
- Linux `arch/arm64/kernel/armv8_deprecated.c` — 未定義命令フック + `ldxr/stxr` による SWP エミュレーションの先例

LSE エミュレーションを公開している既存プロジェクトは、検索した範囲では見つからなかった。
