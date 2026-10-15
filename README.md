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

## 組み込み (T210 + Windows) — winx (steck0714/winx) を土台にする

ドライバでは不可能 (Windows カーネル自身が LSE を含み、フックできない)。
T210 で edk2 を実用的に使える現状唯一の系統として [steck0714/winx](https://github.com/steck0714/winx)
(imbushuo/NintendoSwitchPkg 派生, EDK2 202402) を読み、それを足場に設計した。

### winx のコードから確認できた事実

- ブート連鎖は hekate → BL31(ATF) → BL33(UEFI)。`Tools/Bl33/bl33_stub.S` のコメントによると
  BL31 は BL33 を `0xAA000000` に **EL2 (SPSR_EL2T)** で入れる。UEFI Shell までの到達は実機(Erista)報告あり。
  ただし EL の値そのものを実測した記録はコード上に無い。
- bl33.bin = 4 KiB スタブ + FD。スタブは DAIF マスクと `SPSel` だけして FD へ分岐する。
  `make-bl33.py` が `STUB_SIZE=0x1000` / FD は stub+0x1000 を静的検証している。
  EL2 → EL1 へ落とす処理はプラットフォーム側に見当たらない (grep)。
- ACPI FADT は `ARM_PSCI_COMPLIANT` のみで `PSCI_USE_HVC` が無い → Windows は PSCI を **SMC** で呼ぶ。
- MADT は GICC ×4 (MPIDR 0–3) → 4 コア。GIC は GICv2 系の構成。
- `MemoryMap.h`: `0xAA000000` から 1 MiB が bl33 領域 (`EfiBootServicesCode` = ExitBootServices 後に解放される)。
  `0xFE000000–0x100000000` は TZ carveout で触れない。
- 著者の注記: **Windows は未起動**、BootMgr/WinPE 未試行、ACPI は CPU/GIC/timer/UART のみ、
  Cortex-A57 では Win10 ARM64 か Win11 23H2 以前を使う (LSE 問題。本プロジェクトの動機と一致)。
- ライセンスは GPL-2.0-only (hekate 由来コードを含むため)。bl33.bin に組み込んで配布するなら
  対応ソースの提供義務が及ぶ。lse-shim コア単体のライセンスは別途決める必要あり (法的助言ではない)。

### 案A (EL2 常駐ハイパーバイザ) の具体化 — **以下は仮説、未検証**

1. **UEFI ごと EL1 に落とし、EL2 は常駐させる。** 入口は `bl33_stub.S` (EL2 で入る唯一の場所)。
   EL2 初期化 (`HCR_EL2`, `VBAR_EL2`, Stage-2) → `ERET` で EL1 の FD へ。以降 UEFI も Windows も EL1。
   UEFI を EL2 のままにすると、winload が EL2 のままカーネルに渡す／自力で EL1 へ落とす可能性があり、
   EL2 常駐が成立しない (Windows が実際どの EL で起動するかは未確認)。
   常駐領域は ExitBootServices 後も解放されない予約領域として MemoryMap に追加する。
   4 KiB スタブ制約があるので、EL2 コードは FD 内の別モジュール化か `make-bl33.py` の改修が要る。
2. **Stage-2 で恒等写像**し、Windows の EL1 例外ベクタ (VBAR_EL1) の同期例外スロット
   (+0x200 現EL / +0x400 下位EL) を含むページを実行不可 (XN) にする。
3. **発火**: Undefined を EL1 が受けると ESR_EL1/ELR_EL1/SPSR_EL1 を設定してベクタ先頭を fetch →
   Stage-2 命令アボートで EL2 へ。EL2 が ELR_EL1 の命令を読んで `lse_emulate` し、成功なら
   `ELR_EL2=ELR_EL1+4, SPSR_EL2=SPSR_EL1` で ERET (ゲストのハンドラは走らない)。
   LSE 以外の Undefined は XN を一時解除して 1 命令シングルステップ → 再武装し、ゲストに処理させる。
4. **VBAR_EL1 の発見**: `HCR_EL2.TVM` の対象外でトラップできない。EL2 が制御を得た機会 (SMC トラップ等) に読む。
5. **SMP**: FADT が SMC なので `HCR_EL2.TSC=1` で EL1 の SMC を捕獲し、PSCI `CPU_ON` のエントリを
   自前の EL2 トランポリンに差し替えて各コアの EL2 を初期化する。
6. 同じ仕組みでユーザーモード (EL0) の LSE (ベクタ +0x400) も救える。

### 実機で最初に確認すべきこと (UART 115200 8N1 / winx は右レールのコネクタ)

- bl33 起動直後の `CurrentEL` が本当に 2 か。
- ATF が secondary を `CPU_ON` したとき、何 EL で入るか (EL2 に入らないと各コアの EL2 初期化が成立しない)。
- 単純な EL2 スタブ (EL1 へ ERET して UEFI が従来通り起動するだけ) を挟んでも winx が起動するか。
- Windows がどの EL でカーネルを起動するか (`bcdedit ... debug on` + シリアル WinDbg が使える)。
- 24H2 が LSE 以外に要求する機能 (PAN/RDM/VHE 等) が何か。

その他の障害: T210 には Windows 用 GPU ドライバ (WDDM) が無く、ACPI も CPU/GIC/timer/UART のみ。
LSE を解決しても SD/USB/表示/入力ドライバは未整備 (winx の README 参照)。
まずは Win10 ARM64 / Win11 23H2 以前で winx を起動させるほうが先。

## 参考にした GitHub 上のコード

- QEMU `target/arm/tcg/a64.decode` — `@atomic` / `CAS` / `CASP` のエンコード
- Linux `arch/arm64/include/asm/atomic_ll_sc.h` — LL/SC ループの形
- steck0714/winx — T210 向け EDK2 (hekate→BL31→BL33)。ブート連鎖・メモリマップ・ACPI 構成の把握に使用
- Linux `arch/arm64/kernel/armv8_deprecated.c` — 未定義命令フック + `ldxr/stxr` による SWP エミュレーションの先例

LSE エミュレーションを公開している既存プロジェクトは、検索した範囲では見つからなかった。
