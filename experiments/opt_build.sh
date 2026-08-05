#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# opt_build.sh —— 卷2 第3章 Q1：把 opt_lab.c 用 4 種優化等級各編一次並比較
# 在 ROCK 5B 上執行： cd ~/exp/ch12 && ./opt_build.sh
set -u
KDIR=/lib/modules/$(uname -r)/build
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "### 內核本身的優化等級（zcat /proc/config.gz）"
zcat /proc/config.gz | grep -E "^CONFIG_CC_OPTIMIZE"
echo "### 內核 Makefile 怎麼決定 -O（本機 kernel headers）"
grep -n "O2\|Os\|CC_OPTIMIZE" "$KDIR/Makefile" | grep -i "optimize" | head -8
echo

hdr() { printf "%-6s %-9s %-8s %-9s %-8s %-9s %-11s %s\n" \
	"-O" ".text" "指令數" "堆疊框" "呼叫數" "內聯數" "fbreg/總變數" "說明"; }

row() {
	local lvl=$1
	mkdir -p "$WORK/$lvl"
	cp opt_lab.c "$WORK/$lvl/"
	cat > "$WORK/$lvl/Makefile" <<EOF
obj-m += opt_lab.o
ccflags-y += -g -$lvl
EOF
	if ! make -C "$KDIR" M="$WORK/$lvl" modules >"$WORK/$lvl/log" 2>&1; then
		printf "%-6s 編譯失敗：%s\n" "$lvl" "$(grep -m1 -E 'error|Error' "$WORK/$lvl/log")"
		return
	fi
	local o="$WORK/$lvl/opt_lab.o"
	local text insns frame calls inlined optout
	text=$(size -A "$o" | awk '$1==".text"{print $2}')
	# 只看 opt_lab_work 這一個函式
	insns=$(objdump -d --disassemble=opt_lab_work "$o" | grep -cE "^\s+[0-9a-f]+:")
	frame=$(objdump -d --disassemble=opt_lab_work "$o" | \
		grep -oE "(sub[[:space:]]+sp, sp, #0x[0-9a-f]+|\[sp, #-[0-9]+\]!)" | \
		head -1 | grep -oE "[0-9a-fx]+" | tail -1)
	frame=${frame:-0}
	calls=$(objdump -d --disassemble=opt_lab_work "$o" | grep -cE "\sbl\s")
	inlined=$(readelf --debug-dump=info "$o" 2>/dev/null | grep -c "DW_TAG_inlined_subroutine")
	# GDB 眼中：有幾個區域變數是「固定堆疊槽」(DW_OP_fbreg)，
	# 其餘是 multi-location／constant／stack_value —— 出了 PC 範圍就是 <optimized out>
	optout=$(gdb -batch -ex "info scope opt_lab_work" "$o" 2>/dev/null | \
		awk '/^Symbol /{t++} /DW_OP_fbreg/{f++} END{printf "%d/%d", f+0, t+0}')
	printf "%-6s %-9s %-8s %-9s %-8s %-9s %-11s\n" \
		"$lvl" "$text" "$insns" "$frame" "$calls" "$inlined" "$optout"
}

echo "### opt_lab_work() 在四種優化等級下的產出"
hdr
for l in O0 O1 O2 Os; do row $l; done
echo
echo "### -O0 與 -O2 的 opt_lab_work() 反組譯對照"
for l in O0 O2; do
	echo "--- -$l ---"
	objdump -d --disassemble=opt_lab_work "$WORK/$l/opt_lab.o" 2>/dev/null | sed -n '7,60p'
done
echo
echo "### GDB 眼中的區域變數（info scope opt_lab_work）"
for l in O0 O2; do
	echo "--- -$l ---"
	gdb -batch -ex "info scope opt_lab_work" "$WORK/$l/opt_lab.o" 2>/dev/null | head -20
done
echo
echo "### 行號表（GDB 單步時游標的順序）"
for l in O0 O2; do
	echo "--- -$l : opt_lab_work 的行號表 ---"
	readelf --debug-dump=decodedline "$WORK/$l/opt_lab.o" 2>/dev/null | \
		awk '/opt_lab.c/{p=1} p' | sed -n '1,30p' | awk '{print $2, $3}' | tr '\n' ' '
	echo
done
echo
echo "### -O0 編不動內核的實例（-DO0_BREAK）"
for l in O0 O2; do
	mkdir -p "$WORK/brk$l"; cp opt_lab.c "$WORK/brk$l/"
	cat > "$WORK/brk$l/Makefile" <<EOF
obj-m += opt_lab.o
ccflags-y += -g -$l -DO0_BREAK
EOF
	if make -C "$KDIR" M="$WORK/brk$l" modules >"$WORK/brk$l/log" 2>&1; then
		echo "-$l : 編譯成功"
	else
		echo "-$l : 編譯失敗 →"
		grep -E "error:" "$WORK/brk$l/log" | head -6 | sed 's/^/      /'
	fi
done
