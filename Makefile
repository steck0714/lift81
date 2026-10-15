SHELL := /bin/bash
# aarch64 クロスビルド + qemu-user での差分テスト
CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc
OBJDUMP := $(CROSS)objdump
QEMU    ?= qemu-aarch64

# コア: ARMv8.0 (Cortex-A57) 命令のみ。FP/NEON不使用 (EL2/カーネルのトラップ文脈で安全に使うため)
CORE_FLAGS := -O2 -mcpu=cortex-a57 -mgeneral-regs-only -mno-outline-atomics \
              -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra
# テスト: glibc静的リンク (v8.0)。LSE命令はテスト内でだけ target("+lse") で使う
TEST_FLAGS := -O1 -march=armv8-a -mno-outline-atomics -static -pthread -Wall -Wextra

all: lse_emul.o test_lse

lse_emul.o: lse_emul.c lse_emul.h
	$(CC) $(CORE_FLAGS) -c $< -o $@

test_lse: test_lse.c lse_emul.c lse_emul.h
	$(CC) $(TEST_FLAGS) -mgeneral-regs-only -c lse_emul.c -o lse_emul_t.o
	$(CC) $(TEST_FLAGS) test_lse.c lse_emul_t.o -o $@

# コアがLSE命令を1つも含まないこと (= T210でそのまま動くこと) を確認
check: lse_emul.o
	@if $(OBJDUMP) -d lse_emul.o | grep -E '\s(ld(add|clr|eor|set|smax|smin|umax|umin)[a-z]*|swp[a-z]*|cas[a-z]*|stadd[a-z]*|stclr[a-z]*|steor[a-z]*|stset[a-z]*)\s' ; then \
	  echo "NG: core contains LSE instructions"; exit 1; else echo "OK: core contains no LSE instructions"; fi

test: test_lse check
	@set -e; \
	$(QEMU) -cpu max        ./test_lse native > out_native.txt 2> err_native.txt || { echo "FAIL: native 実行に失敗 (rc=$$?)"; cat err_native.txt; exit 1; }; \
	$(QEMU) -cpu cortex-a57 ./test_lse emul   > out_emul.txt   2> err_emul.txt   || { echo "FAIL: emul 実行に失敗 (rc=$$?)"; cat err_emul.txt; exit 1; }; \
	grep -q '^TOTAL' out_native.txt || { echo "FAIL: native の出力が不完全 (空出力での偽PASS防止)"; exit 1; }; \
	grep -q '^TOTAL' out_emul.txt   || { echo "FAIL: emul の出力が不完全 (空出力での偽PASS防止)"; exit 1; }; \
	nh=$$(sed -n 's/^trap hits=//p' err_native.txt); eh=$$(sed -n 's/^trap hits=//p' err_emul.txt); \
	[ "$$nh" = "0" ] || { echo "FAIL: native でトラップが発生 (hits=$$nh)"; exit 1; }; \
	[ "$${eh:-0}" -gt 0 ] || { echo "FAIL: emul でエミュレータが一度も呼ばれていない"; exit 1; }; \
	diff <(grep -v '^mode=' out_native.txt) <(grep -v '^mode=' out_emul.txt) > diff.txt \
	  && echo "PASS: native(LSE) と emul(LL/SC) の全結果が一致 (groups=$$(grep -c 'cases=' out_emul.txt), traps=$$eh)" \
	  || { echo "FAIL: 差分あり (diff.txt)"; head diff.txt; exit 1; }

clean:
	rm -f *.o test_lse out_*.txt err_*.txt diff.txt
.PHONY: all check test clean
