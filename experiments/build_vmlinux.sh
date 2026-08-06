#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# build_vmlinux.sh —— 在板子上重建一個「和正在跑的核心位址完全相符、但帶 DWARF」的 vmlinux
#
# 為什麼做得到？這塊板子的 6.1.115+ 就是在板子上用 Debian gcc 12.2 編的
# （`cat /proc/version` 看得到 radxa@rock-5b），原始碼樹還在 ~/disk/kernel-source，
# 而且 CONFIG_RELOCATABLE=y 但沒有 KASLR → 組態與編譯器一致，符號位址就會一致。
#
# 產物用途：objdump -d vmlinux（= crash 的 dis）、gdb、faddr2line，以及餵給 crash。
# 用法：scp 到板子 → sudo bash build_vmlinux.sh（約 25 分鐘，log 在 ~/kbuild.log）
#
# 完成後務必比對（crash 會逐字檢查版本橫幅）：
#   strings ~/kbuild-src/vmlinux | grep -m1 "^Linux version"   vs   cat /proc/version
set -x
exec > /home/radxa/kbuild.log 2>&1
date
# 1. 複製一份原始碼樹（不動 /lib/modules/6.1.115+/build 指到的那棵）
if [ ! -d /home/radxa/kbuild-src ]; then
	cp -a /home/radxa/disk/kernel-source /home/radxa/kbuild-src || exit 1
fi
cd /home/radxa/kbuild-src || exit 1
# 2. 用「正在跑的核心的組態」當基礎，只加 DWARF
printf '+' > .scmversion
zcat /proc/config.gz > .config
./scripts/config -d DEBUG_INFO_NONE -e DEBUG_INFO_DWARF4 -d DEBUG_INFO_REDUCED -d DEBUG_INFO_SPLIT -d DEBUG_INFO_BTF
make olddefconfig
grep -E "^CONFIG_DEBUG_INFO" .config
make kernelrelease
# 3. 版本橫幅必須和 /proc/version 一模一樣，否則 crash 會拒絕
export KBUILD_BUILD_TIMESTAMP='Mon Apr 27 08:30:35 UTC 2026'
export KBUILD_BUILD_USER=radxa
export KBUILD_BUILD_HOST=rock-5b
export KBUILD_BUILD_VERSION=1	# 少了它橫幅會變成 "# SMP"（缺 build number），crash 就不認
time make -j8 vmlinux
ls -la vmlinux
date
echo BUILD_DONE_RC=$?
