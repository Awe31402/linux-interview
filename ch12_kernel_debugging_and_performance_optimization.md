# 卷2 第 3 章 內核調試與性能優化 — 高頻面試題解答

> **實驗平台**：Radxa ROCK 5B（Rockchip RK3588），`192.168.68.58`（帳密皆為 `radxa`）
> **CPU**：4×Cortex-A55（cpu0~3）＋ 4×Cortex-A76（cpu4~7）
> **OS / Kernel**：Debian 12 bookworm，`Linux rock-5b 6.1.115+ #1 SMP aarch64`
> **Bootloader**：U-Boot 2017.09（Rockchip），`androidboot.fwver=…bl31-v1.45,uboot-17.09-33-f-08/06/2024`
> **板上就有這顆核心的完整原始碼**：`/lib/modules/6.1.115+/build` → `~/disk/kernel-source`（6.1.115），
> 所以下面所有「核心原始碼行號」都可以在板子上直接 `grep` 驗證。
>
> **關鍵組態**（`zcat /proc/config.gz`）：
> ```
> CONFIG_CC_OPTIMIZE_FOR_SIZE=y       # ← 這顆核心是 -Os 編的，不是 -O2
> CONFIG_DEBUG_INFO_NONE=y            # 沒有 DWARF，vmlinux 也沒裝到板子上
> CONFIG_RELOCATABLE=y                # CONFIG_RANDOMIZE_BASE is not set  ← 有重定位、沒 KASLR
> CONFIG_SLUB_DEBUG=y                 # CONFIG_SLUB_DEBUG_ON is not set   ← 框架在、預設關
> # CONFIG_KASAN is not set           # CONFIG_PROVE_LOCKING is not set   ← 沒有 lockdep
> CONFIG_DEBUG_SPINLOCK=y             # CONFIG_DEBUG_MUTEXES is not set
> # CONFIG_DETECT_HUNG_TASK is not set  # CONFIG_SOFTLOCKUP_DETECTOR is not set
> CONFIG_DYNAMIC_DEBUG=y              CONFIG_JUMP_LABEL=y
> CONFIG_FTRACE=y  CONFIG_FUNCTION_TRACER=y  CONFIG_TRACEPOINTS=y  CONFIG_KPROBES=y
> CONFIG_MESSAGE_LOGLEVEL_DEFAULT=4   CONFIG_CONSOLE_LOGLEVEL_DEFAULT=7
> # CONFIG_PANIC_ON_OOPS is not set   ← oops 不會 panic，可以安全重複實驗
> CONFIG_STRICT_MODULE_RWX=y  CONFIG_ARM64_VA_BITS=48  CONFIG_PGTABLE_LEVELS=4
> ```
>
> **書目對照**
> - 《奔跑吧 Linux 內核》（第二版）卷 2 第 3 章 —
>   `books/running-linux-kernel/running-kernel-2-txt/10_第3章_内核调试与性能优化.txt`
>   （下稱「奔跑吧卷2 §3.x」，行號以該純文字檔為準）
> - 《奔跑吧 Linux 內核》（第二版）卷 1 第 2 章 §2.x「連結器腳本 / 加載地址」—
>   `books/running-linux-kernel/running-kernel-1-txt/10_第2章_ARM64在Linux内核中的实现.txt:2328-2392`
> - 本專案的核心原始碼樹（6.1.84，與板上 6.1.115 同一條 Rockchip 分支）
>   `/home/awe/disk/yocto-rockchip-sdk/build/tmp/work-shared/rockchip-rk3588-rock-5b/kernel-source`
> - 本專案的 U-Boot 原始碼樹（2017.09 Rockchip）`kernel-source/u-boot/`
> - 相關筆記：📝 [ch02 ARM64 在 Linux 內核中的實現](./ch02_arm64_in_linux_kernel.md)、
>   📝 [ch10 併發與同步](./ch10_concurrency_and_synchronization.md)、
>   📝 [ch11 中斷管理](./ch11_interrupt_management.md)
>
> **實驗程式碼**：全部在 [`notes/experiments/`](./experiments/)，一鍵重現腳本
> [`experiments/ch12_run_all.sh`](./experiments/ch12_run_all.sh)。

---

## 書是 5.0 + QEMU virt 寫的，本機是 6.1.115 + RK3588 真硬體 —— 12 處差異

| # | 書上（5.0 / QEMU virt / VA_BITS=48） | 本機實測（6.1.115 / ROCK 5B） | 題號 |
|---|------|------|------|
| 1 | 內核預設 `-O2` | 本機 `CONFIG_CC_OPTIMIZE_FOR_SIZE=y` → **`-Os`**（Makefile:829-835） | Q1 |
| 2 | 「把 Makefile 的 O2 改成 O0」 | 6.1 的 arm64 **`-O0` 直接編不過**：`cmpxchg()` → `system_uses_lse_atomics()` → `asm goto` 約束失敗（`alternative-macros.h:232`），實測錯誤訊息在 Q1 | Q1 |
| 3 | `KIMAGE_VADDR = 0xFFFF000010000000`、`TEXT_OFFSET=0x80000` | 6.1 **`TEXT_OFFSET` 已刪除**（5.10, commit `120dc60d0bdb`），`KIMAGE_VADDR = MODULES_END = 0xffff800008000000` | Q2、Q7 |
| 4 | 重定位在 `__primary_switch` 裡「用 `ldr x8,=__primary_switched` + `br`」 | 6.1 多了真正的 **`__relocate_kernel()`**（`head.S:706-793`）：逐筆掃 `.rela.dyn` 套用 `R_AARCH64_RELATIVE`，另有 RELR 壓縮格式 | Q7 |
| 5 | `adrp x0, __PHYS_OFFSET` | 6.1 改叫 **`KERNEL_START`**，且 `x23 = adrp(KERNEL_START) & (MIN_KIMG_ALIGN-1)`（`head.S:801-802`）—— 沒 KASLR 也可能有非零位移 | Q7 |
| 6 | slub 報告 `Redzone overwritten` | 6.1 分左右：**`Right Redzone overwritten`**；double free 走到 `slab_err()` 印 **`Slab has 0 allocated objects but 1 are to be freed`**（`mm/slub.c:2855`），不是書上的 `Object already free`（那條在 `slub.c:1403`，另一條路徑才會走到） | Q9 |
| 7 | `slabinfo.c` 在 `tools/vm/` | 6.1 還在 `tools/vm/slabinfo.c`（6.4 起才搬到 `tools/mm/`） | Q9 |
| 8 | 打開 `CONFIG_PROVE_LOCKING` 看 lockdep 報告 | 本機**沒有 lockdep、沒有 hung_task、沒有 softlockup**，死鎖不會有任何人喊叫 —— 只能自己量。實測見 Q10/Q11 | Q10、Q11 |
| 9 | `printk` 預設等級 `7 4 1 7` | 本機 **`4 4 1 7`**（`quiet` + `loglevel=4` 在 cmdline 裡） | Q12 |
| 10 | 動態輸出只在 `/sys/kernel/debug/dynamic_debug/control` | 6.1 另有 **`/proc/dynamic_debug/control`**（5.7, commit `239a5791ffd5`，不必掛 debugfs），本機共 **6217 條** | Q13 |
| 11 | oops 的 `pstate: 60000005 (nZCv daif -PAN -UAO)` | 6.1 多了 `-TCO -DIT -SSBS BTYPE=--`；`Internal error: Oops: 96000044` 變成 **`Oops: 0000000096000044`**（64 位元 ESR），並多印 `EC = 0x25: DABT`、`FSC = 0x04` 的**文字解釋** | Q14 |
| 12 | `%pf` 印函式指標 | `%pf`/`%pF` 已刪除（5.5, commit `9af7706492f9`），改用 **`%pS/%ps`**；`%p` 預設**雜湊**（4.15, commit `ad67b74d2469`），要真位址得用 `%px` 或 `no_hash_pointers` | Q12 |

---

## 目錄

| # | 題目 | 實機關鍵證據 |
|---|------|-------------|
| [1](#q1) | 使用 GCC 的 O0 優化選項來編譯內核有什麼優勢 | 同一函式 **-O0 是 37 條指令 / 6 個變數全在堆疊；-O2 是 18 條 / 0 個**；-O2 行號表在 **同一個位址 0x8 上掛了 9 個不同行號**（游標亂跳的真身）；**-O0 在本機真的編不過**，錯誤訊息在下面 |
| [2](#q2) | 什麼是加載地址、運行地址和鏈接地址 | U-Boot `kernel_addr_r=0x00400000` → `/proc/iomem` Kernel code `0x00410000` → `_stext` 虛擬 `0xffff800008010000`，**三個位址一路對上** |
| [3](#q3) | 什麼是位置無關/有關的匯編指令 | 把同一段機器碼搬到別的位址再執行：**`adr`/`bl` 的回傳值跟著搬家，`ldr x0,=sym`/`blr` 回傳值文風不動** |
| [4](#q4) | 什麼是重定位 | 三層同一個把戲：使用者態 PIE 的 **`R_AARCH64_RELATIVE addend=e18`**、vmlinux 的 **262510 筆 `R_AARCH64_RELATIVE`**、模組載入時把 `bl 0 <_printk>` **就地改寫成 `95fbdf99`** |
| [5](#q5) | 為什麼要刻意讓三個位址不一樣 | RK3588 實測：U-Boot 鏈接在 `0x00200000`、跑起來搬到 DRAM 頂端、核心被放到 `0x00400000`、DTB `0x08300000`、initrd `0x0a200000` |
| [6](#q6) | U-Boot 啟動時重定位如何實現 | `relocate_64.S` 的 `copy_loop` + `fixloop`（只認 `R_AARCH64_RELATIVE`）；本機 u-boot ELF 有 **4934 筆**；`crt0_64.S` 用 `adr lr,relocation_return; add lr,lr,gd->reloc_off` 玩「回到搬家後的自己」 |
| [7](#q7) | 內核啟動時映像重定位如何實現 | `head.S:706-793 __relocate_kernel`；實測 **`kaslr_offset()=0`、`kimage_voffset=0xffff800007c00000`**、`_text` 實體位址 `0x400000`（= 2 MB 對齊，所以 `x23=0`） |
| [8](#q8) | 如何在內核代碼中添加一個跟蹤點 | 樹外 `TRACE_EVENT()` 模組：`events/tp_lab/` 長出來、`ID: 2182`；並拍到 **static key 把 `d503201f`(NOP) 改成 `14000002`(B)** 的那一刻 |
| [9](#q9) | slub_debug 可以檢測哪些類型的記憶體錯誤 | 不改 cmdline、不重開機，用私有 kmem_cache 重現 **Right Redzone overwritten / Poison overwritten / 重複釋放 / Objects remaining**，`slabinfo` 顯示 `Redzoning: On Poisoning: On Tracking: On` |
| [10](#q10) | 什麼是死鎖 | AA 自旋鎖 **500 ms 內 `spin_trylock` 失敗 92,897,122 次**；AA mutex **睡死 4.17 秒**，被 SIGKILL 叫醒後還「假裝」拿到了鎖（附原始碼解釋） |
| [11](#q11) | 常見的死鎖有哪幾種 | AB-BA 兩條 kthread 各卡 4.77 秒；書上第二個例子（mutex vs `cancel_delayed_work_sync`）**完整重現**：`dl_book` 進 D 狀態，堆疊正是 `__flush_work → __cancel_work_timer → cancel_delayed_work_sync` |
| [12](#q12) | 什麼是 printk 輸出等級 | 8 個等級全印一遍 + `dmesg -x`/`dmesg -r` 對照；本機 `/proc/sys/kernel/printk = 4 4 1 7` |
| [13](#q13) | 如何使用內核的動態輸出技術 | 全系統 **6217 條**動態語句；示範 `module/func/file+line` 三種選擇器、`+pflmt` 五個旗標、`insmod dyndbg=+pfl` |
| [14](#q14) | 如何分析一個 oops 錯誤日誌 | 真 oops：`ESR=0x96000044`（寫）vs `0x96000004`（讀）；`Code:` 行 → `decodecode` → `str w0,[x1]`；`faddr2line` 直接指到 **oops_lab.c:46** |

---

## 一鍵重現全部實驗

```bash
cd notes/experiments && ./ch12_run_all.sh 192.168.68.58
```

或手動：

```bash
HOST=192.168.68.58
ssh radxa@$HOST 'mkdir -p ~/exp/ch12'
scp notes/experiments/{addr_probe,reloc_mod,opt_lab,tp_lab,slub_lab,dl_lab,printk_lab,dyndbg_lab,oops_lab}.c \
    notes/experiments/{tp_lab_trace.h,ksym.h,pic_demo.c,pic_asm.S,opt_build.sh} radxa@$HOST:~/exp/ch12/
ssh radxa@$HOST 'cd ~/exp/ch12 && make'
```

> ⚠️ **注意**：`oops_lab` 在 `module_init` 裡觸發 oops 之後，模組會永遠卡在
> `MODULE_STATE_COMING`（`lsmod` 看得到、`rmmod` 移不掉），同名模組也無法再 insmod。
> 所以腳本會複製成 `oops_m1..oops_m7` 七份不同名字的模組。重開機即可清乾淨。

---

<a name="q1"></a>
## Q1：使用 GCC 的「O0」優化選項來編譯內核有什麼優勢？

### 書上怎麼說（奔跑吧卷2 §3.1.1，行 47-70）

> 「使用 GDB 單步調試內核時會出現光標亂跳並且無法輸出有些變量的值（如出現 `<optimized out>`）……
> 把 linux-5.0 根目錄下的 Makefile 中的 `O2` 改成 `O0`，但是這樣編譯會有問題，我們為此做了一些修改……
> 使用 `O0` 編譯內核會導致內核運行性能下降，因此僅僅是為了方便單步調試內核而使用。」

一句話：**`-O0` 唯一的優勢就是「原始碼和機器碼一一對應」，讓 GDB 好用。**

### 本機驗證

實驗模組 [`experiments/opt_lab.c`](./experiments/opt_lab.c)，
腳本 [`experiments/opt_build.sh`](./experiments/opt_build.sh) 用四種優化等級各編一次同一個
`opt_lab_work()`（一個含 `static inline` 呼叫、8 次迴圈、5 個區域變數的函式）：

```bash
ssh radxa@192.168.68.58 'cd ~/exp/ch12 && ./opt_build.sh'
```

```
### 內核本身的優化等級
CONFIG_CC_OPTIMIZE_FOR_SIZE=y          ← 本機核心是 -Os 編的，不是書上的 -O2

### opt_lab_work() 在四種優化等級下的產出
-O     .text     指令數 堆疊框 呼叫數 內聯數 fbreg/總變數
O0     460       37       64        1        1         6/6
O1     216       17       0         0        2         0/6
O2     228       18       0         0        2         0/6
Os     216       17       0         0        2         0/6
```

四個欄位分別對應四件事：

| 欄位 | -O0 | -O2 | 對調試的意義 |
|---|---|---|---|
| 指令數 | 37 | 18 | -O0 多一倍，**每個 C 敘述都獨立成幾條指令** |
| 堆疊框 | 64 B | 0 | -O0 每個變數都有固定堆疊槽 |
| 呼叫數 | 1（`bl scale`）| 0 | -O2 把 `static inline scale()` **內聯掉了**，斷點打不進去 |
| `fbreg/總變數` | 6/6 | 0/6 | -O0 六個區域變數全都是 `DW_OP_fbreg`（固定堆疊位置）；-O2 一個都不是 |

**(a) 為什麼會 `<optimized out>`** —— `gdb -batch -ex "info scope opt_lab_work" opt_lab.o`：

```
--- -O0 ---                              --- -O2 ---
Symbol n is a complex DWARF expression:  Symbol n is multi-location:
     0: DW_OP_fbreg -36                    Base address 0x8  Range 0x8-0x20: a variable in $x0
, length 4.                                Range 0x20-0x48: a variable in $x4
Symbol acc is a complex DWARF expression:Symbol acc is multi-location:
     0: DW_OP_fbreg -20                    Base address 0x8  Range 0x8-0x24: the constant 0
, length 4.                                Range 0x30-0x38: a complex DWARF expression:
Symbol i ... DW_OP_fbreg -16                  0: DW_OP_breg3 0 [$x3]
Symbol tmp_a ... DW_OP_fbreg -12              2: DW_OP_breg0 0 [$x0]
Symbol tmp_b ... DW_OP_fbreg -8               4: DW_OP_plus
Symbol tmp_c ... DW_OP_fbreg -4               5: DW_OP_stack_value
```

`-O0`：每個變數 = 「frame base + 固定偏移」，**函式全程都讀得到**。
`-O2`：`acc` 只在 PC 落在 `0x8-0x24`、`0x30-0x38` 這兩段範圍內才有位置，
其餘位址 GDB 只能回答 `<optimized out>`；而且它根本不在記憶體裡，
是「用 x3 + x0 算出來的一個值」（`DW_OP_stack_value`）。

**(b) 為什麼游標會亂跳** —— 行號表（`readelf --debug-dump=decodedline`，格式為「行號 位址」）：

```
-O0： 24 0x34  25 0x40  25 0x4c  25 0x54  26 0x58  30 0x68  31 0x74  33 0x78  34 0x84
      37 0x98  37 0x9c  38 0xa0  39 0xb0  39 0xbc  39 0xc4  37 0xd0  37 0xdc  41 0xe8  42 0xec
                                     ↑ 位址單調遞增，行號幾乎也是

-O2： 30 0x8  31 0x8  32 0x8  33 0x8  34 0x8  35 0x8  37 0x8  37 0x8  30 0x8
      33 0xc  34 0x10  37 0x14  34 0x18  31 0x1c  31 0x20  39 0x28  38 0x30
      23 0x30  25 0x30  25 0x30  39 0x30  37 0x30  39 0x34 ...
      ↑ 同一個位址 0x8 對應到 30/31/32/33/34/35/37 七個不同行號；
        0x30 那個位址甚至對應到 38/23/25/39/37 五行（23、25 是被內聯進來的 scale()）
```

**這就是「游標亂跳」的物理原因**：一條指令屬於好幾行原始碼，
GDB 每 `stepi` 一次就得挑一個行號顯示，看起來就在來回跳。

### 但是：本機的 6.1 內核 `-O0` **根本編不過**

```bash
# 模組裡放一個 cmpxchg() 和一個 BUILD_BUG_ON，用 -O0 編：
ssh radxa@192.168.68.58 'W=$(mktemp -d); cp ~/exp/ch12/opt_lab.c $W/;
  printf "obj-m += opt_lab.o\nccflags-y += -g -O0 -DO0_BREAK\n" > $W/Makefile;
  make -C /lib/modules/$(uname -r)/build M=$W modules'
```

```
In function ‘alternative_has_feature_likely’,
    inlined from ‘system_uses_lse_atomics’ at ./arch/arm64/include/asm/lse.h:21:9,
    inlined from ‘__cmpxchg_case_mb_32’ at ./arch/arm64/include/asm/cmpxchg.h:129:1:
./arch/arm64/include/asm/alternative-macros.h:232:9: error: ‘asm’ operand 0 probably does
    not match constraints [-Werror]
./arch/arm64/include/asm/alternative-macros.h:232:9: error: impossible constraint in ‘asm’
cc1: all warnings being treated as errors
make[1]: *** [scripts/Makefile.build:250: opt_lab.o] Error 1

-O2 : 編譯成功                       ← 同一份程式碼，只差一個優化等級
```

原因鏈：`cmpxchg()` → `system_uses_lse_atomics()` → `alternative_has_feature_likely()` →
`asm goto` + ALTERNATIVE 巨集。`asm goto` 的 label 操作數要求編譯器做基本的流程分析，
`-O0` 不做，於是「不可能的約束」。這比書上 5.0 時代「要改一些地方」更嚴重：

**結論（面試講法）**

1. **`-O0` 的唯一優勢是可調試性**：不內聯、不消除變數、行號表單調，
   GDB 單步不跳、變數不會 `<optimized out>`。實測 -O0 六個區域變數 100 % 可見，-O2 是 0 %。
2. 代價：**指令數 ×2、堆疊用量從 0 變 64 B、效能大幅下降**，
   而且核心大量依賴「編譯器會把常數摺疊 / 死碼消除」的慣用法（`BUILD_BUG_ON`、
   `asm goto`、`__builtin_constant_p`、`cmpxchg` 依 size 展開），`-O0` 直接編不動。
3. 所以實務上**只在 QEMU + 專門的調試核心上用 `-O0`**；真板子上要調試，
   正確作法是保留 `-O2/-Os` 但打開 `CONFIG_DEBUG_INFO`（本機是
   `CONFIG_DEBUG_INFO_NONE=y`，所以 `/boot` 下連 `vmlinux` 都沒有），
   再靠 ftrace / kprobe / oops 解碼來定位（見 Q8、Q14）。

---

<a name="q2"></a>
## Q2：什麼是加載地址、運行地址和鏈接地址？

### 書上定義（奔跑吧卷2 §3.1.5，行 505-511）

> - **加載地址**：存儲代碼的物理地址。
> - **運行地址**：指程序運行時的地址。
> - **鏈接地址**：在編譯鏈接時指定的地址，編程人員設想將來程序要運行的地址。
>   程序中所有標號的地址在鏈接後便確定了……用 `objdump` 反匯編查看的就是鏈接地址。

（卷 1 §2.x 行 2340-2360 補充了連結器腳本裡 `AT>` 的用法：
`address` 是虛擬/運行地址，`AT` 後面才是加載地址。）

### 本機把三個位址全部量出來

**① 鏈接地址** —— 編譯期就寫死在 `vmlinux` 裡（`arch/arm64/kernel/vmlinux.lds.S:154-157`
`. = KIMAGE_VADDR; .head.text : { _text = .; }`）：

```bash
ssh radxa@192.168.68.58 'cd ~/exp/ch12 && sudo insmod addr_probe.ko; sudo dmesg'
```

```
--- [1] 編譯期就決定的「鏈接地址」骨架 ---
  VA_BITS              = 48
  PAGE_OFFSET          = 0xffff000000000000  (線性映射起點)
  KIMAGE_VADDR         = 0xffff800008000000  (核心映像 _text 的鏈接地址)
  MODULES_VADDR        = 0xffff800000000000
  MODULES_END          = 0xffff800008000000  (模組區 128 MB)
  VMALLOC_START        = 0xffff800008000000
```

**② 加載地址** —— U-Boot 把 Image 讀到哪個實體位址。
RK3588 的預設環境變數寫死在 `u-boot/include/configs/rk3588_common.h:72-78`：

```c
#define ENV_MEM_LAYOUT_SETTINGS \
	"fdt_addr_r=0x08300000\0" \
	"kernel_addr_r=0x00400000\0" \
	"kernel_addr_c=0x05480000\0" \
	"ramdisk_addr_r=0x0a200000\0"
```

板子上的 `/proc/iomem` 一字不差地印證了這三個位址：

```bash
ssh radxa@192.168.68.58 'sudo head -12 /proc/iomem'
```
```
00200000-efffffff : System RAM
  00410000-01b2ffff : Kernel code      ← _stext..__init_begin，落在 kernel_addr_r=0x400000 之後
  01b30000-0224ffff : reserved
  02250000-026cffff : Kernel data
  08300000-08343fff : reserved         ← fdt_addr_r  = 0x08300000（DTB）
  0a200000-0b535fff : reserved         ← ramdisk_addr_r = 0x0a200000（initrd）
```

**③ 運行地址** —— 打開 MMU 之後，核心真正執行的虛擬位址：

```
--- [2] 執行期才知道的「加載地址 / 運行地址」---
  memstart_addr(PHYS_OFFSET) = 0x0000000000000000
  kimage_voffset       = 0xffff800007c00000   (= 虛擬 - 實體)
  kimage_vaddr         = 0xffff800008000000
  kaslr_offset()       = 0x0000000000000000   (CONFIG_RANDOMIZE_BASE=n → 位移 0)
  PHYS(_text)          = 0x0000000000400000   ← 加載地址！和 kernel_addr_r 完全相同

--- [3] 各段的鏈接地址 ↔ 實體加載地址 ---
  _stext             虛擬 = 0xffff800008010000   實體 = 0x0000000000410000   距 KIMAGE_VADDR +0x10000
  _etext             虛擬 = 0xffff800009070000   實體 = 0x0000000001470000   距 KIMAGE_VADDR +0x1070000
  __init_begin       虛擬 = 0xffff800009730000   實體 = 0x0000000001b30000
  __primary_switch   虛擬 = 0xffff800009065400   實體 = 0x0000000001465400
  __relocate_kernel  虛擬 = 0xffff8000090653b4   實體 = 0x00000000014653b4
```

### 三個位址的關係（本機實際數字）

```
             鏈接地址                    加載地址              運行地址
_text     0xffff800008000000   ←→   0x00400000   ←→   0xffff800008000000
_stext    0xffff800008010000   ←→   0x00410000   ←→   0xffff800008010000
                                     ↑ U-Boot 的 kernel_addr_r
          （objdump vmlinux 看到的）  （DDR 上的位置）    （MMU 打開後的 PC）

換算關係： 虛擬 = 實體 + kimage_voffset = 實體 + 0xffff800007c00000
驗算：     0x400000 + 0xffff800007c00000 = 0xffff800008000000 ✓
```

**本機的特殊情況**：因為 `CONFIG_RANDOMIZE_BASE=n` 且 Image 剛好被放在
2 MB 對齊的 `0x400000`，所以 **運行地址 == 鏈接地址**（`kaslr_offset()=0`）。
真正「運行地址 ≠ 鏈接地址」的階段只有 **MMU 打開之前的那幾十條指令**（見 Q7）。

**面試講法**：加載地址是「東西被放在 DDR 的哪裡」（bootloader 決定，本機 `0x400000`）；
鏈接地址是「連結器以為它會在哪裡跑」（`vmlinux.lds.S` 決定，本機 `0xffff800008000000`）；
運行地址是「PC 實際指到哪裡」。三者不同時，就得靠**位置無關代碼（Q3）撐到重定位（Q4）完成**。

---

<a name="q3"></a>
## Q3：什麼是位置無關的匯編指令？什麼是位置有關的匯編指令？

### 書上定義（奔跑吧卷2 §3.1.5，行 519-530）

> - **位置無關代碼**：無論運行地址和鏈接地址相等或者不相等，該指令都能正常運行。
>   像 `BL`、`B`、`MOV` 屬於位置無關指令……它們的地址域是基於 PC 值的相對偏移尋址，相當於 `[pc+offset]`。
> - **位置有關代碼**：該指令的執行是與內存地址有關的……ARM 匯編裡面通過絕對跳轉修改 PC 值為當前鏈接地址的值：`ldr pc, =on_sdram`。

### 本機驗證：把同一段機器碼搬到別的位址再執行

[`experiments/pic_asm.S`](./experiments/pic_asm.S) 裡有 6 個小函式，全部塞在
`blob_start..blob_end` 這一段連續的程式碼裡。
[`experiments/pic_demo.c`](./experiments/pic_demo.c) 用 `mmap(PROT_EXEC)` 配一塊新記憶體，
把整段 blob `memcpy` 過去（**這一步就是 bootloader 搬 image 幹的事**），
再分別呼叫「原地那份」和「搬家那份」的同名函式：

```bash
ssh radxa@192.168.68.58 'cd ~/exp/ch12 && gcc -O2 -o pic_demo pic_demo.c pic_asm.S && ./pic_demo'
```

```
blob 鏈接地址(執行檔載入後) L = 0xaaaadb770e18 .. 0xaaaadb770e7c (100 bytes)
blob 複製後的運行地址       R = 0xffff86e69000 (delta = 0x5554ab6f81e8)

函式        類型     指令                     在 L 執行      在 R 執行      判定
------------------------------------------------------------------------------------------
pic_where   PIC      adr x0, pic_where        aaaadb770e18   ffff86e69000   回傳值跟著搬家 → 位置無關
abs_where   non-PIC  ldr x0, =pic_where       aaaadb770e18   aaaadb770e18   回傳值不變 → 位置有關
pic_call    PIC      bl callee                aaaadb770e30   ffff86e69018   回傳值跟著搬家 → 位置無關
abs_call    non-PIC  ldr x1,=callee; blr x1   aaaadb770e30   aaaadb770e30   回傳值不變 → 位置有關
pic_data    PIC      adrp/add :lo12:gvar      aaaadb790100   ffff86e89100   PC 相對但指向沒搬家的資料

--- 非 PIC 函式的「文字池」內容 ---
abs_where 的指令 @L: 58000040 d65f03c0      （ldr x0,[pc,#8] ; ret）
abs_where 文字池 @L = 0xaaaadb770e18
abs_where 文字池 @R = 0xaaaadb770e18   ← 複製過來也還是同一個絕對位址
pic_where 的鏈接地址 = 0xaaaadb770e18
```

四個結論，每一條都是量出來的：

1. **`adr x0, label`（0x10000000）是 PC 相對**：搬到 `R` 執行就回傳 `R`。
   —— 這正是 head.S 在 MMU 打開前唯一能用的取址方式。
2. **`bl callee` 是 PC 相對（imm26 × 4，±128 MB）**：搬家後呼叫的是**搬家那份 callee**。
3. **`ldr x0, =sym` 是位置有關**：組譯器把 `sym` 的**絕對鏈接位址**放進緊跟其後的
   「文字池（literal pool）」。`ldr` 本身雖然是 PC 相對讀取，
   但**讀到的內容是一個寫死的絕對位址**，所以搬到哪都回傳老位址 `0xaaaadb770e18`。
   `blr x1` 於是「跳回原來那份程式碼」—— 如果原本那份已經不在了（例如 Nor Flash 被關掉），就當場掛掉。
4. **`adrp + add :lo12:` 也是 PC 相對（±4 GB）**，但它指向的是**資料**。
   程式碼搬了、資料沒搬，位址就跟著錯位（`aaaadb790100` → `ffff86e89100`）。
   所以「位置無關」的正確說法是：**指令本身跟載入位置無關，但整個 image 必須整包一起搬**。

### ARM64 指令的分類速查（面試用）

| 類別 | 指令 | 範圍 | 為什麼 |
|---|---|---|---|
| **位置無關** | `b` / `bl` | ±128 MB | imm26 是 PC 相對的字偏移 |
| | `b.cond` / `cbz` / `tbz` | ±1 MB / ±32 KB | 同上 |
| | `adr` | ±1 MB | PC + imm21 |
| | `adrp` (+`add :lo12:`) | ±4 GB | (PC & ~0xfff) + imm21<<12 |
| | `ldr x0, label`（literal） | ±1 MB | PC 相對**讀取**（讀到什麼要另外看） |
| | `mov` 立即數 | — | 根本不碰位址 |
| **位置有關** | `ldr x0, =sym` | — | 文字池裡是**絕對位址**，需要重定位 |
| | `.quad sym` / 函式指標表 | — | 資料裡的絕對位址（`R_AARCH64_ABS64`） |
| | 跳板 `ldr x8,=f; br x8` | — | 這正是核心用來「跳進鏈接地址」的手段 |

核心裡最經典的一行就在 `head.S:822`：

```asm
	ldr	x8, =__primary_switched	// 位置有關：取的是鏈接地址（虛擬位址）
	adrp	x0, KERNEL_START	// 位置無關：取的是運行位址（實體位址）
	br	x8			// 一跳跳進虛擬位址空間 —— 重定位完成
```

---

<a name="q4"></a>
## Q4：什麼是重定位？

### 書上定義（奔跑吧卷2 §3.1.5，行 530-531）

> 「當通過 LDR 指令跳轉到鏈接地址處執行時，運行地址就等於鏈接地址了。
> 這個過程叫作『重定位』。在重定位之前，程序只能執行和位置無關的一些匯編代碼。」

書上這句話講的是「**控制流的重定位**」（跳到鏈接地址去跑）。
更完整的定義還包含「**資料的重定位**」：把 image 裡所有寫死的絕對位址，
按照 `實際載入位址 − 鏈接位址` 這個差值統統加上去。這件事在 ARM64 上有一個統一的名字：
**`R_AARCH64_RELATIVE` 重定位項**。

### 本機驗證：同一個把戲出現在三個層次

#### ① 使用者空間（PIE 執行檔，由 `ld.so` 做）

```bash
ssh radxa@192.168.68.58 'cd ~/exp/ch12 && readelf -r pic_demo | head -6
  objdump -d --disassemble=abs_where pic_demo | tail -4'
```
```
Relocation section '.rela.dyn' at offset 0x528 contains 35 entries:
  Offset          Info           Type              Sym. Value   Sym. Name + Addend
000000000e28  000000000403 R_AARCH64_RELATIV                    e18   ← 文字池那 8 個位元組
000000000e68  000000000403 R_AARCH64_RELATIV                    e30

 e20:	58000040 	ldr	x0, e28 <abs_where+0x8>
 e24:	d65f03c0 	ret
 e28:	00000e18 	.word	0x00000e18          ← 檔案裡存的是「鏈接位址 0xe18」
```

執行時 `ld.so` 把「載入基址」加到 `0xe28` 這 8 個位元組上，
於是 `abs_where()` 回傳 `0xaaaadb770e18` 而不是 `0xe18`。
**演算法：`*(u64*)(offset + base) = addend + base`。** 記住這行，下面兩個層次一模一樣。

對照組：`gcc -no-pie` 編出來的同一支程式**沒有這兩筆重定位**（`.rela.dyn` 只剩 3 筆
`R_AARCH64_GLOB_DAT`），因為位址在連結時就定死了。

#### ② 核心映像（`vmlinux`，由 `head.S` 自己做）

```bash
readelf -h vmlinux | grep Type      # → Type: DYN (Shared object file)  ← 核心是 PIE！
readelf -r vmlinux | grep -c R_AARCH64_RELATIV
```
```
Type:  DYN (Shared object file)
262510                              ← 26 萬筆相對重定位（本機同源碼樹編的 vmlinux）
```

這些項目被連結器收進 `.rela.dyn`，並由連結腳本標上起訖符號
（`arch/arm64/kernel/vmlinux.lds.S:259-262`）：

```c
	.rela.dyn : ALIGN(8) {
		__rela_start = .;
		*(.rela .rela*)
		__rela_end = .;
	}
```

`head.S:706-724` 的 `__relocate_kernel()` 就是一個 24 行的迴圈，逐筆套用（詳見 Q7）。

#### ③ 核心模組（`.ko`，由 module loader 做）—— 本機最好觀察的一層

模組**每次載入的位址都不一樣**，所以重定位是實打實在發生的。
[`experiments/reloc_mod.c`](./experiments/reloc_mod.c) 把自己的 `reloc_probe_site()`
在**記憶體裡的機器碼**dump 出來，和 `.o` 檔裡的留白對照：

```bash
ssh radxa@192.168.68.58 'cd ~/exp/ch12 && sudo insmod reloc_mod.ko; sudo dmesg
  objdump -dr --disassemble=reloc_probe_site reloc_mod.o'
```

```
=== 重定位「前」：objdump -dr reloc_mod.o（檔案裡）===
0000000000000000 <reloc_probe_site>:
   0:	d503201f 	nop
   4:	d503201f 	nop
   8:	a9bf7bfd 	stp	x29, x30, [sp, #-16]!
   c:	90000000 	adrp	x0, 0 <reloc_probe_site>       ← 位址欄位全是 0
			c: R_AARCH64_ADR_PREL_PG_HI21	.data
  10:	91000002 	add	x2, x0, #0x0
			10: R_AARCH64_ADD_ABS_LO12_NC	.data
  1c:	90000000 	adrp	x0, 0 <reloc_probe_site>
			1c: R_AARCH64_ADR_PREL_PG_HI21	.rodata.str1.1
  24:	94000000 	bl	0 <_printk>                    ← 呼叫目標也是 0
			24: R_AARCH64_CALL26	_printk

=== 重定位「後」：模組載入後從記憶體讀回來 ===
模組載入位址(運行地址) core_layout.base = ffff80000111c000, size = 16384
reloc_probe_site  = ffff80000111c004
reloc_gvar        = ffff80000111e018
_printk           = ffff800009013e8c
  reloc_probe_site +0x00: aa1e03e9              ← mov x9,x30（ftrace 的 patchable entry）
  reloc_probe_site +0x04: d503201f
  reloc_probe_site +0x08: a9bf7bfd
  reloc_probe_site +0x0c: d0000000  ADRP x0, 0xffff80000111e000   ← 填好了！
  reloc_probe_site +0x10: 91006002  ADD  x2, x0, #0x18            ← :lo12: = 0x018
  reloc_probe_site +0x1c: b0000000  ADRP x0, 0xffff80000111d000
  reloc_probe_site +0x20: 91015000  ADD  x0, x0, #0x54
  reloc_probe_site +0x24: 95fbdf99  BL  imm26=0x1fbdf99 → 0xffff800009013e8c <_printk+0x0/0x90>
```

驗算：`0xffff80000111e000 + 0x18 = 0xffff80000111e018` = `reloc_gvar` ✓
`0xffff80000111c028 + (0x1fbdf99 << 2) = 0xffff800009013e8c` = `_printk` ✓

做這件事的是 `arch/arm64/kernel/module.c: apply_relocate_add()`，
`readelf -r reloc_mod.ko` 可以看到這個模組總共有 25 筆待處理的重定位項。

### 面試講法

> 重定位 = **把「連結器以為的位址」換成「實際載入後的位址」**，有兩個層面：
> ① 控制流：用一條位置有關的 `ldr x8,=sym; br x8` 跳進鏈接地址（書上講的）；
> ② 資料：掃描 `.rela.dyn`，對每一筆 `R_AARCH64_RELATIVE` 做
> `*(offset+delta) += delta`。
> ARM64 上 U-Boot（`relocate_64.S`）、Linux 核心（`head.S:__relocate_kernel`）、
> `ld.so`（PIE）三者用的**是同一個演算法、同一個重定位型別**；
> 模組載入則是 `apply_relocate_add()` 處理 `R_AARCH64_CALL26 / ADR_PREL_PG_HI21 / …`。

---

<a name="q5"></a>
## Q5：在實際項目開發中，為什麼要刻意設置加載地址、運行地址以及鏈接地址不一樣？

### 書上的理由（奔跑吧卷2 §3.1.5，行 532-543）

> 「如果所有代碼都在 ROM（或 Nor Flash）中執行，那麼鏈接地址可以與加載地址相同；
> 而在實際項目應用中，往往想要把程序加載到 DDR 內存中，DDR 的訪問速度比 ROM 快很多，
> 而且容量也大。但是礙於加載地址的影響，不可能直接達到這一步，
> 所以思路就是讓程序的加載地址等於 ROM 起始地址，而鏈接地址等於 DDR 中某一處的起始地址……」

### RK3588 的真實啟動鏈（本機量到的四個位址）

RK3588 上電後的順序是 **BootROM → ddr.bin(TPL) → SPL → ATF(BL31) → U-Boot → Linux**，
每一級都在解同一個問題：「我現在能用的記憶體，跟我最終想跑的地方，不是同一塊」。

| 階段 | 鏈接地址 | 實際跑在哪 | 為什麼不一樣 | 證據 |
|---|---|---|---|---|
| BootROM | 片內 ROM | ROM | 唯一能執行的東西 | — |
| TPL(ddr.bin) | 片內 SRAM `0x0010f000` | SRAM | **DDR 還沒初始化**，只能用 SRAM | `/proc/iomem`: `0010f000-0010f0ff : 10f000.sram` |
| U-Boot proper | `CONFIG_SYS_TEXT_BASE = 0x00200000` | 先在 `0x200000`，**再自己搬到 DRAM 頂端** | 要把低位址整片讓給核心、DTB、initrd | `u-boot/include/configs/rk3588_common.h:30`；`readelf -h u-boot` → `Entry point 0x200000` |
| Linux | `KIMAGE_VADDR = 0xffff800008000000` | 加載在 `0x00400000`，開 MMU 後跑在 `0xffff8000_08000000` | **核心要用虛擬位址**（線性映射、模組區、vmalloc 全都在高位址） | `kernel_addr_r=0x00400000`；`/proc/iomem` Kernel code `0x00410000` |

三個「刻意不一樣」的實際好處，本機都看得到：

1. **速度與容量**：U-Boot 本體 ~1 MB，SRAM 只有 256 B~64 KB 級別的窗口，非放 DDR 不可。
2. **騰出空間**：U-Boot 把自己搬到 DRAM 最頂端（`gd->relocaddr = gd->ram_top` 之後一路往下扣，
   `u-boot/common/board_f.c:317-430`），**低位址整片留給核心**。
   本機核心佔 `0x00400000-0x026cffff`、DTB `0x08300000`、initrd `0x0a200000`
   —— 如果 U-Boot 還賴在 `0x200000` 不走，這些就會踩到它。
3. **核心必須用虛擬位址鏈接**：`_stext = 0xffff800008010000`，
   而它被加載在實體 `0x00410000`。這不是「可以選」，是**架構要求**：
   核心態的 TTBR1 只認高位址（`VA_BITS=48` 下 `0xffff_0000_0000_0000` 以上），
   而且模組區 `MODULES_VADDR=0xffff800000000000` 必須在核心 `_text` 的 ±128 MB 內
   （這樣模組的 `bl` 才跳得到核心函式）—— 實測本機模組載在
   `0xffff80000111c000`，離 `_stext` **-111 MB**，剛好卡在 `R_AARCH64_CALL26` 的
   ±128 MB 上限內。

> **面試加分點**：本機 `addr_probe.ko` 印出「模組 → 核心 `_stext` 距離 = -111 MB」，
> 這就是為什麼 arm64 的 `MODULES_END` 被硬性定義成 `KIMAGE_VADDR`、
> 模組區只有 128 MB —— 一旦超出，`bl` 就得走 PLT 跳板（`arch/arm64/kernel/module-plts.c`）。

---

<a name="q6"></a>
## Q6：在 U-Boot 啟動時重定位是如何實現的？

### 書上（奔跑吧卷2 §3.1.5，行 536-544，圖 3.14）

> 「程序先從 ROM 中啟動，最先啟動的部分要實現代碼複製功能（把整個 ROM 代碼複製到 DDR 內存中），
> 並通過 LDR 指令來跳轉到 DDR 內存中，也就是在鏈接地址裡運行（B 指令沒法實現這個跳轉）。」

### 本機 U-Boot 2017.09（Rockchip）的實際實現

板子上跑的就是這個版本（cmdline: `uboot-17.09-33-f-08/06/2024`），
原始碼在 `kernel-source/u-boot/`。整個流程分四步：

**① 算出要搬到哪裡** —— `common/board_f.c`（`board_init_f` 的一串 init 函式）：

```c
 317:	gd->relocaddr = gd->ram_top;      /* 從 DRAM 最頂端開始往下扣 */
 339:	gd->relocaddr -= (reg << 10);     /* reserve_round_4k / mmu table / video / trace ... */
 359:	gd->relocaddr -= gd->arch.tlb_size;
 430:	gd->relocaddr -= gd->mon_len;     /* ← reserve_uboot()：U-Boot 本體 */
	gd->relocaddr &= ~(4096 - 1);
	...
	gd->reloc_off = gd->relocaddr - CONFIG_SYS_TEXT_BASE;   /* setup_reloc() */
```

**② 跳過去，而且要「跳到搬家後的自己」** —— `arch/arm/lib/crt0_64.S`：

```asm
	bl	board_init_f
	...
	adr	lr, relocation_return		/* lr = 現在這份的 relocation_return */
	ldr	x9, [x18, #GD_RELOC_OFF]	/* x9 = gd->reloc_off */
	add	lr, lr, x9			/* lr = 搬家後那份的 relocation_return ← 關鍵！ */
	ldr	x0, [x18, #GD_RELOCADDR]	/* x0 = gd->relocaddr（目的地）*/
	b	relocate_code
relocation_return:			/* ← 從這裡開始，已經是在新位址上執行了 */
	bl	c_runtime_cpu_setup
```

**③ 複製 + 修重定位表** —— `arch/arm/lib/relocate_64.S`（就是書上圖 3.14 的本體）：

```asm
ENTRY(relocate_code)
	adrp	x1, __image_copy_start		/* 位置無關：取「現在」的位址 */
	add	x1, x1, :lo12:__image_copy_start
	subs	x9, x0, x1
	b.eq	relocate_done			/* 已經在對的位置就不用搬 */

	ldr	x1, _TEXT_BASE			/* 位置有關：取「鏈接」的位址 */
	subs	x9, x0, x1			/* x9 = Link to copy offset = delta */

copy_loop:				/* ← 書上說的「代碼複製功能」 */
	ldp	x10, x11, [x1], #16
	stp	x10, x11, [x0], #16
	cmp	x1, x2
	b.lo	copy_loop

	/* Fix .rela.dyn relocations */
	adrp	x2, __rel_dyn_start
	adrp	x3, __rel_dyn_end
fixloop:				/* ← 資料的重定位，和 Q4 完全同一套演算法 */
	ldp	x0, x1, [x2], #16	/* (x0,x1) <- (要改的位址, 重定位型別) */
	ldr	x4, [x2], #8		/* x4 <- addend */
	and	x1, x1, #0xffffffff
	cmp	x1, #R_AARCH64_RELATIVE
	bne	fixnext
	add	x0, x0, x9		/* 位址 += delta */
	add	x4, x4, x9		/* 內容 += delta */
	str	x4, [x0]
fixnext:
	cmp	x2, x3
	b.lo	fixloop
```

`relocate_64.S:34-42` 的註解把 Q3 的重點講得很白：

> *"Don't `ldr x1, __image_copy_start` here, since if the code is already running at an
> address other than it was linked to, that instruction will load the **relocated** value…
> To correctly apply relocations, we need to know the **linked** value."*

**④ 清 BSS 之後才敢用絕對位址** —— `crt0_64.S` 的註解直接寫著：

```asm
	ldr	x0, =__bss_start	/* this is auto-relocated! */
	ldr	x1, =__bss_end		/* this is auto-relocated! */
```

這兩行就是「位置有關指令」，但它們的文字池已經被 `fixloop` 修好了，所以能用。

### 量化證據

```bash
readelf -h u-boot | grep -E "Type|Entry"
readelf -r u-boot | grep -c R_AARCH64_RELATIV
```
```
Type:  EXEC (Executable file)
Entry point address:  0x200000          ← = CONFIG_SYS_TEXT_BASE（rk3588_common.h:30）
4934                                    ← 4934 筆相對重定位要在 fixloop 裡跑一遍
```

> **U-Boot vs 核心的差別**：U-Boot 是 `EXEC` + `-pie` 產生的 `.rela.dyn`（自己搬自己）；
> 核心是 `DYN`（真 PIE）+ `CONFIG_RELOCATABLE`。兩者的 `fixloop` 是同一段邏輯，
> 差別只在核心多支援 RELR 壓縮格式（`head.S:727-789`）。

---

<a name="q7"></a>
## Q7：在內核啟動時內核映像重定位是如何實現的？

### 書上（奔跑吧卷2 §3.1.5，行 546-570，圖 3.15）

> 「這個重定位過程在 `__primary_switch` 匯編函數中完成。啟動 MMU 之後，
> 通過 `ldr` 指令把 `__primary_switched` 函數的鏈接地址加載到 x8 暫存器，
> 然後通過 `br` 指令跳轉到 `__primary_switched` 的鏈接地址處。」

書上（5.0）只講了「控制流重定位」。6.1 多了完整的**資料重定位**。

### 本機 6.1.115 的 `head.S`（板上 `~/disk/kernel-source/arch/arm64/kernel/head.S`）

```asm
SYM_FUNC_START_LOCAL(__primary_switch)                       /* head.S:796 */
	adrp	x1, reserved_pg_dir
	adrp	x2, init_idmap_pg_dir
	bl	__enable_mmu                     /* ① 先開 MMU（用恆等映射 idmap）*/
#ifdef CONFIG_RELOCATABLE
	adrp	x23, KERNEL_START                /* ② x23 = 實體載入位址 */
	and	x23, x23, MIN_KIMG_ALIGN - 1     /*    只留 2 MB 內的偏移 */
#ifdef CONFIG_RANDOMIZE_BASE
	bl	__pi_kaslr_early_init            /*    有 KASLR 時再 or 上隨機位移 */
	orr	x23, x23, x0
#endif
#endif
	bl	clear_page_tables
	bl	create_kernel_mapping            /* ③ 建立真正的核心映射 */
	adrp	x1, init_pg_dir
	load_ttbr1 x1, x1, x2
#ifdef CONFIG_RELOCATABLE
	bl	__relocate_kernel                /* ④ 資料重定位 ← 5.0 沒有這一步 */
#endif
	ldr	x8, =__primary_switched          /* ⑤ 位置有關：取鏈接地址 */
	adrp	x0, KERNEL_START
	br	x8                               /* ⑥ 控制流重定位：跳進虛擬位址 */
SYM_FUNC_END(__primary_switch)
```

`__relocate_kernel()` 本體（`head.S:706-724`），**和 U-Boot 的 `fixloop` 一模一樣**：

```asm
SYM_FUNC_START_LOCAL(__relocate_kernel)
	adr_l	x9,  __rela_start        /* 連結腳本標出來的 .rela.dyn 起訖 */
	adr_l	x10, __rela_end
	mov_q	x11, KIMAGE_VADDR        /* 鏈接時的虛擬基址 */
	add	x11, x11, x23            /* 實際的虛擬基址 = 鏈接基址 + 位移 */
0:	cmp	x9, x10
	b.hs	1f
	ldp	x12, x13, [x9], #24      /* x12=offset, x13=info */
	ldr	x14, [x9, #-8]           /* x14=addend */
	cmp	w13, #R_AARCH64_RELATIVE
	b.ne	0b                       /* 只認 RELATIVE，其它跳過 */
	add	x14, x14, x23            /* 內容 += delta */
	str	x14, [x12, x23]          /* 寫回 (位址 += delta) */
	b	0b
1:	/* CONFIG_RELR：壓縮格式，一個 64 bit 位元圖可以編碼 63 筆 */
```

### 本機實測：位移是多少？

```bash
ssh radxa@192.168.68.58 'cd ~/exp/ch12 && sudo insmod addr_probe.ko; sudo dmesg | grep -E "kaslr|kimage|PHYS"'
```
```
  kimage_voffset       = 0xffff800007c00000
  kimage_vaddr         = 0xffff800008000000
  kaslr_offset()       = 0x0000000000000000   (CONFIG_RANDOMIZE_BASE=n → 位移 0)
  PHYS(_text)          = 0x0000000000400000
```

`kaslr_offset()` 的定義就是 `kimage_vaddr - KIMAGE_VADDR`
（`arch/arm64/include/asm/memory.h:201-204`），實測為 **0**，原因有二：

1. `CONFIG_RANDOMIZE_BASE=n` → 沒有隨機位移；
2. U-Boot 把 Image 放在 `0x00400000`，**剛好是 2 MB 對齊**
   → `x23 = adrp(KERNEL_START) & (MIN_KIMG_ALIGN-1) = 0`。

也就是說，本機這 26 萬筆 `R_AARCH64_RELATIVE` **每一筆都加了 0**——
迴圈照跑，結果不變。這正是為什麼在真板子上不容易「看到」核心重定位的效果，
所以我在 Q4 改用**模組**（每次載入位址都不同）來展示同一套機制。

### 完整時間軸（本機數字）

```
U-Boot: booti 0x00400000 - 0x08300000
   ↓ PC = 0x00400000（實體），MMU 關閉，運行地址 ≠ 鏈接地址
primary_entry → __create_page_tables …    ← 只能用 adr/adrp/b/bl（位置無關）
   ↓
__primary_switch:
   __enable_mmu       ← MMU 開了，但用的是 idmap（實體=虛擬），PC 還在 0x400000 附近
   x23 = 0x400000 & (2MB-1) = 0
   create_kernel_mapping  ← 建立 0xffff800008000000 → 0x400000 的映射
   __relocate_kernel  ← 掃 .rela.dyn，262510 筆，delta=0
   ldr x8, =__primary_switched  ; x8 = 0xffff800009736b90（鏈接地址）
   br  x8             ← ★ 重定位完成，PC 從 0x14xxxxx 一躍到 0xffff8000_09xxxxxx
   ↓
__primary_switched: 設定 sp、清 BSS、記下 kimage_voffset、start_kernel()
```

`__primary_switched` 裡記錄 `kimage_voffset` 的那段（`head.S:433`）：

```asm
	ldr_l	x4, kimage_vaddr		// Save the offset between
	sub	x4, x4, x0			// the kernel virtual and
	str_l	x4, kimage_voffset, x5		// physical mappings
```

實測 `kimage_voffset = 0xffff800008000000 - 0x400000 = 0xffff800007c00000` ✓

---

<a name="q8"></a>
## Q8：如何在內核代碼中添加一個跟蹤點？

### 書上（奔跑吧卷2 §3.2.5，行 1140-1310）

在 `include/trace/events/sched.h` 裡寫 `TRACE_EVENT(sched_stat_minvruntime, …)`
六個參數（name / proto / args / struct / assign / print），
在 `update_curr()` 裡呼叫 `trace_sched_stat_minvruntime(...)`，**重新編譯內核**，
然後 `/sys/kernel/debug/tracing/events/sched/sched_stat_minvruntime/` 就出現了。

### 本機驗證：不重編內核也能加跟蹤點（樹外 TRACE_EVENT）

[`experiments/tp_lab_trace.h`](./experiments/tp_lab_trace.h) +
[`experiments/tp_lab.c`](./experiments/tp_lab.c)。
三個必備樣板（跟書上樹內寫法唯一的差別）：

```c
#undef TRACE_SYSTEM
#define TRACE_SYSTEM tp_lab              /* → events/tp_lab/ 這個目錄名 */

TRACE_EVENT(tp_lab_alloc,
	TP_PROTO(struct task_struct *tsk, u64 size, unsigned long addr),
	TP_ARGS(tsk, size, addr),
	TP_STRUCT__entry(__array(char, comm, TASK_COMM_LEN) __field(pid_t, pid)
			 __field(u64, size) __field(unsigned long, addr)),
	TP_fast_assign(memcpy(__entry->comm, tsk->comm, TASK_COMM_LEN);
		       __entry->pid = tsk->pid; __entry->size = size; __entry->addr = addr;),
	TP_printk("comm=%s pid=%d size=%llu addr=0x%lx", ...)
);

#undef  TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .             /* ← 樹外模組必須自己指路 */
#define TRACE_INCLUDE_FILE tp_lab_trace
#include <trace/define_trace.h>
```
`.c` 裡 `#define CREATE_TRACE_POINTS` 之後再 include 這個標頭，Makefile 加 `ccflags-y += -I$(src)`。

```bash
ssh radxa@192.168.68.58 'cd ~/exp/ch12 && sudo insmod tp_lab.ko
  sudo ls /sys/kernel/debug/tracing/events/tp_lab/
  sudo cat /sys/kernel/debug/tracing/events/tp_lab/tp_lab_alloc/format'
```

```
/sys/kernel/debug/tracing/events/tp_lab/:
enable  filter  tp_lab_alloc  tp_lab_big_alloc

name: tp_lab_alloc
ID: 2182
format:
	field:unsigned short common_type;	offset:0;	size:2;	signed:0;
	field:unsigned char common_flags;	offset:2;	size:1;	signed:0;
	field:unsigned char common_preempt_count;	offset:3;	size:1;	signed:0;
	field:int common_pid;	offset:4;	size:4;	signed:1;

	field:char comm[16];	offset:8;	size:16;	signed:0;
	field:pid_t pid;	offset:24;	size:4;	signed:1;
	field:u64 size;	offset:32;	size:8;	signed:0;
	field:unsigned long addr;	offset:40;	size:8;	signed:0;

print fmt: "comm=%s pid=%d size=%llu addr=0x%lx", REC->comm, REC->pid, ...
```

**和書上完全一樣的版面**（4 個 common 欄位 + 自訂欄位），
只是本機 ID 是 2182（書上 208，因為本機跟蹤點多得多）。

抓資料：

```bash
echo 1 | sudo tee /sys/kernel/debug/tracing/events/tp_lab/tp_lab_alloc/enable
sudo cat /sys/kernel/debug/tracing/trace
```
```
# entries-in-buffer/entries-written: 7/7   #P:8
#           TASK-PID     CPU#  |||||  TIMESTAMP  FUNCTION
          tp_lab-10677   [006] .....  1759.685348: tp_lab_alloc: comm=tp_lab pid=10677 size=576  addr=0xffff00010434fb80
          tp_lab-10677   [006] .....  1759.891983: tp_lab_alloc: comm=tp_lab pid=10677 size=1088 addr=0xffff00010434fb80
          tp_lab-10677   [006] .....  1759.891983: tp_lab_big_alloc: big size=1088
          tp_lab-10677   [006] .....  1760.098515: tp_lab_alloc: comm=tp_lab pid=10677 size=1600 addr=0xffff00010434fb80
          tp_lab-10677   [006] .....  1760.098519: tp_lab_big_alloc: big size=1600
          tp_lab-10677   [006] .....  1760.305101: tp_lab_alloc: comm=tp_lab pid=10677 size=64   addr=0xffff00010434fb80
```

注意 `tp_lab_big_alloc` 只在 `size >= 1024` 時出現 —— 那是用
`TRACE_EVENT_CONDITION()` + `TP_CONDITION(size >= 1024)` 定義的（書上行 1306-1307 提到）。

### 加分題：跟蹤點關閉時的代價是多少？—— 實測 NOP ↔ B 的改寫

跟蹤點沒打開時**不是「if (enabled)」判斷**，而是一條 `NOP`（static key / jump label）。
模組把自己 `tp_lab_hit()` 的機器碼 dump 出來：

```
--- 跟蹤點還沒 enable ---            --- echo 1 > .../enable 之後 ---
  +0x00: aa1e03e9                     +0x00: aa1e03e9    ← mov x9,x30（ftrace 進入點）
  +0x04: d503201f  <-- NOP             +0x04: d503201f
  +0x08: a9bd7bfd                      +0x08: a9bd7bfd
  ...                                  ...
  +0x20: d503201f  <-- NOP             +0x20: 14000002  <-- B（跳去呼叫 probe）★
  +0x24: 14000012                      +0x24: 14000012
```

**`0xd503201f`（NOP）被就地改寫成 `0x14000002`（B +8）** ——
這就是 `CONFIG_JUMP_LABEL=y` 下 `static_branch_unlikely()` 的實作
（`arch/arm64/kernel/jump_label.c: arch_jump_label_transform()`），
所以「跟蹤點關閉時的開銷 = 一條 NOP ≈ 0」。

還有一個容易被忽略的細節，本機也拍到了：**把 ftrace 的 enable 關掉之後，
那條 B 還在**。因為模組同時用 `register_trace_tp_lab_alloc(my_probe, NULL)`
掛了自己的 probe —— static key 是**引用計數**的，只要還有任何一個消費者，
跟蹤點就保持打開。這也順帶示範了跟蹤點的第二種用法（不經過 ftrace ring buffer，
直接掛 callback，`perf`、eBPF、`blktrace` 都是這樣接上去的）。

### 三種「加觀測點」的方式（面試對比）

| 方式 | 要重編內核嗎 | 關閉時開銷 | 適用 |
|---|---|---|---|
| `TRACE_EVENT()` 樹內（書上） | 要 | 1 條 NOP | 上游長期維護的觀測點 |
| `TRACE_EVENT()` 樹外模組（本文） | **不用** | 1 條 NOP | 自己的驅動 / 臨時調查 |
| kprobe event（`CONFIG_KPROBE_EVENTS=y`） | 不用，連模組都不用 | 0（沒插樁） | 任意核心函式，但拿不到區域變數語意 |

```bash
# 本機也支援完全不寫程式碼的動態跟蹤點：
echo 'p:myprobe kmem_cache_alloc' | sudo tee /sys/kernel/debug/tracing/dynamic_events
```

---

<a name="q9"></a>
## Q9：slub_debug 可以檢測哪些類型的記憶體錯誤？

> 題目原文是「內存洩漏」，但書上 §3.3 行 1665-1678 講得很清楚，
> slub_debug 抓的是**內存訪問錯誤**，洩漏反而是它最抓不到的一種（見下面第 5 項）。

### 書上列的三種（§3.3.1 行 1675-1678）

> 「訪問已經被釋放的內存、越界訪問、釋放已經釋放過的內存。」

### 本機的處境：框架在、預設關

```bash
ssh radxa@192.168.68.58 'cat /proc/cmdline | tr " " "\n" | grep -i slub; zcat /proc/config.gz | grep SLUB_DEBUG'
```
```
cmdline 沒有 slub_debug
CONFIG_SLUB_DEBUG=y                 ← 程式碼編進去了
# CONFIG_SLUB_DEBUG_ON is not set   ← 但預設不開
```

書上的作法是重編內核 + 改 cmdline 加 `slub_debug=UFPZ`。
**本機不動 cmdline、不重開機**也能做完整實驗：自己建一個帶 debug 旗標的 kmem_cache。
四個旗標和 `slub_debug` 的四個字母是一對一的（`mm/slub.c:1461-1472`）：

| 字母 | 旗標 | 作用 |
|---|---|---|
| `f` | `SLAB_CONSISTENCY_CHECKS` | 各種一致性檢查（double free、freepointer） |
| `z` | `SLAB_RED_ZONE` | 物件前後放 `0xcc/0xbb` 警戒帶 |
| `p` | `SLAB_POISON` | 未用 `0x5a`、已釋放 `0x6b`、結尾 `0xa5` |
| `u` | `SLAB_STORE_USER` | 記下 alloc/free 的呼叫堆疊（就是報告裡的 `Allocated in`/`Freed in`） |

[`experiments/slub_lab.c`](./experiments/slub_lab.c) 就是這樣建 cache 的：

```c
flags = SLAB_RED_ZONE | SLAB_POISON | SLAB_STORE_USER | SLAB_CONSISTENCY_CHECKS;
cache = kmem_cache_create("slub_lab", objsize, 0, flags, NULL);
```

驗證真的開了（和沒開 debug 的 `kmalloc-32` 對照）：

```bash
sudo sh -c 'for f in red_zone poison store_user sanity_checks object_size slab_size objs_per_slab; do
   printf "%-14s slub_lab=%s  kmalloc-32(:0000032)=%s\n" $f \
     "$(cat /sys/kernel/slab/slub_lab/$f)" "$(cat /sys/kernel/slab/:0000032/$f)"; done'
```
```
red_zone       slub_lab=1    kmalloc-32=0
poison         slub_lab=1    kmalloc-32=0
store_user     slub_lab=1    kmalloc-32=0
sanity_checks  slub_lab=1    kmalloc-32=0
object_size    slub_lab=32   kmalloc-32=32
slab_size      slub_lab=128  kmalloc-32=32     ← debug 讓一個 32 B 物件佔用 128 B！
objs_per_slab  slub_lab=32   kmalloc-32=128
```

> 順帶回答另一個常考點：**打開 slub_debug 的代價**。同樣是 32 B 的物件，
> slab_size 從 32 B 變成 128 B（4 倍），因為要塞左右 Redzone + 兩份 `struct track`。
> 而且 `SLAB_MERGE_DEFAULT` 的合併會被關掉（`kmalloc-32` 在本機被合併成 `:0000032`，
> 我們的 cache 則獨立存在）。

書上用的 `slabinfo` 工具本機也能編（`tools/vm/slabinfo.c`，6.1 還在 `tools/vm/`）：

```bash
gcc -O2 -o slabinfo ~/disk/kernel-source/tools/vm/slabinfo.c && sudo ./slabinfo slub_lab
```
```
Slabcache: slub_lab         Aliases:  0 Order :  0 Objects: 1

Sizes (bytes)     Slabs              Debug                Memory
------------------------------------------------------------------------
Object :      32  Total  :       1   Sanity Checks : On   Total:    4096
SlabObj:     128  Full   :       0   Redzoning     : On   Used :      32
SlabSiz:    4096  Partial:       1   Poisoning     : On   Loss :    4064
Loss   :      96  CpuSlab:       0   Tracking      : On   Lalig:      96
```

### 五種錯誤，本機逐一重現

```bash
for m in 1 2 4 8 16; do sudo insmod slub_lab.ko mode=$m; sudo rmmod slub_lab; sudo dmesg; done
```

#### ① 越界訪問（out-of-bounds）→ `Right Redzone overwritten`

```c
p = kmem_cache_alloc(cache, GFP_KERNEL);   /* 32 B */
memset(p, 0x55, 32 + 8);                   /* 多寫 8 B */
kmem_cache_free(cache, p);                 /* ← 釋放時 check_object() 抓到 */
```
```
BUG slub_lab (Tainted: G    B   W  O      ): Right Redzone overwritten
-------------------------------------------------------------------------
0x000000007c75a041-0x00000000ae9da40f @offset=40. First byte 0x55 instead of 0xcc
Allocated in slub_lab_init+0xd8/0x1000 [slub_lab] age=0 cpu=0 pid=11331
 slub_lab_init+0xd8/0x1000 [slub_lab]
 do_one_initcall+0x88/0x1cc
 ...
Slab 0x00000000c5506009 objects=32 used=1 fp=0x00000000bcdcd5c1 flags=0x8000000000000200
Object 0x00000000dffb01eb @offset=8 fp=0x0000000000000000

Redzone  000000006e7c4a6d: cc cc cc cc cc cc cc cc                          ........
Object   00000000dffb01eb: 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55  UUUUUUUUUUUUUUUU
Object   00000000d5073005: 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55  UUUUUUUUUUUUUUUU
Redzone  000000007c75a041: 55 55 55 55 55 55 55 55                          UUUUUUUU   ← 被踩了
Padding  00000000fb1be3a0: 5a 5a 5a 5a 5a 5a 5a 5a                          ZZZZZZZZ
Call trace:
 check_bytes_and_report+0xc4/0x124
 check_object+0x84/0x204
 free_debug_processing+0x174/0x36c
 kmem_cache_free+0x13c/0x1b0
```

一眼就能讀出：**左 Redzone 還是 `cc`、右 Redzone 變成 `55`、`@offset=40` 正好是
物件 32 B 之後**，而 `Allocated in` 直接指出是誰配的。
（6.1 的訊息比書上的 5.0 多了「Right/Left」的區分。）

> 實驗踩到的坑：如果一次越界寫太多（例如書上的 `memset(buf,0x55,200)`），
> 會把 `SLAB_STORE_USER` 的 `struct track` 也蓋掉，於是報告裡的
> `Allocated in 0x5555555555555555` 變成亂碼，還會額外噴一個
> `stack_depot_fetch` 的 WARNING。所以 `slub_lab.ko` 的 `overflow=` 參數預設只越界 8 B。

#### ② 訪問已釋放的記憶體（use-after-free）→ `Poison overwritten`

```c
p = kmem_cache_alloc(cache, GFP_KERNEL);
kmem_cache_free(cache, p);
memset(p, 0x55, 32);          /* ← 蓋掉 POISON_FREE(0x6b) */
```
```
BUG slub_lab (Tainted: G    B   W  O      ): Poison overwritten
0x00000000c8ee3a23-0x0000000067db60ac @offset=8. First byte 0x55 instead of 0x6b
Allocated in slub_lab_init+0x10c/0x1000 [slub_lab] age=0 cpu=1 pid=10797
 ...
Freed in slub_lab_init+0x11c/0x1000 [slub_lab] age=0 cpu=1 pid=10797
 kmem_cache_free+0x13c/0x1b0
 slub_lab_init+0x11c/0x1000 [slub_lab]
 ...
Redzone  00000000adb09aca: bb bb bb bb bb bb bb bb        ← 已釋放物件的 redzone 是 bb
Object   00000000c8ee3a23: 55 55 55 55 ...                ← 應該是 6b 6b 6b...
```

**和書上 §3.3.1 行 1918-1923 的輸出格式完全一致**（`First byte 0x55 instead of 0x6b`），
而且因為有 `SLAB_STORE_USER`，`Allocated in` + `Freed in` 兩份堆疊都在——
這是排查 UAF 最值錢的兩行。

#### ③ 重複釋放（double free）

```
BUG slub_lab (...): Slab has 0 allocated objects but 1 are to be freed
Call trace:
 slab_err+0x98/0xd0
 free_debug_processing+0x12c/0x36c
 kmem_cache_free+0x13c/0x1b0
 slub_lab_init+0x188/0x1000 [slub_lab]
```

> **與書上的差異**：書上 5.0 印的是 `Object already free`。
> 6.1 兩條訊息都在（`mm/slub.c:1403` 是 `Object already free`，
> `mm/slub.c:2855` 是本次觸發的 `Slab has %d allocated objects but %d are to be freed`），
> 走哪一條取決於物件當下在 per-cpu freelist 還是 partial list。

#### ④ 改壞 free pointer

SLUB 把「下一個空閒物件」的指標存在物件本身裡。故意寫入 `0x4141…` 之後，
本機報的是 `Poison overwritten @offset=8 First byte 0x41 instead of 0x6b`
——因為打開 `SLAB_POISON` 時 freepointer 被移到物件外面，先撞到的是 poison。
真正的 `Freepointer corrupt` 檢查在 `mm/slub.c:1162`（`object_err`）和 `1220`（`slab_err`），
在沒有 poison 的情境（例如純 `slub_debug=F`）才會先觸發。

#### ⑤ 記憶體洩漏 —— **slub_debug 抓不到**

```
BUG slub_lab (...): Objects remaining in slub_lab on __kmem_cache_shutdown()
Call trace:
 __kmem_cache_shutdown+0x138/0x244
 kmem_cache_destroy+0x54/0x134
 slub_lab_exit+0x18/0x1000 [slub_lab]
```

只有在**銷毀自己的 cache** 時才會被發現（模組卸載時）。
如果洩漏的是 `kmalloc` 出來的記憶體，slub_debug **完全不會出聲**，
只能從 `/sys/kernel/slab/<cache>/objects` 或 `/proc/slabinfo` 看到數字一直漲。
抓洩漏要用 **`CONFIG_DEBUG_KMEMLEAK`**（本機沒開），或 KASAN 的 quarantine。

### 總結表（面試背這張）

| 錯誤類型 | slub_debug 抓得到嗎 | 訊息 | 靠哪個旗標 | 何時被發現 |
|---|---|---|---|---|
| 越界寫（右） | ✅ | `Right Redzone overwritten` | `Z` | free 時 / `validate` 時 |
| 越界寫（左） | ✅ | `Left Redzone overwritten` | `Z` | 同上 |
| use-after-free | ✅ | `Poison overwritten` | `P` | 下次 alloc/free 時 |
| double free | ✅ | `Object already free` / `Slab has N allocated…` | `F` | 立刻 |
| freepointer 被改壞 | ✅ | `Freepointer corrupt` | `F` | 下次 alloc |
| 用錯 cache 釋放 | ✅ | `Object 0x… does not belong to cache` | `F` | 立刻 |
| **記憶體洩漏** | ❌ | 只有 cache 銷毀時的 `Objects remaining` | — | 幾乎抓不到 → 用 kmemleak |
| 堆疊溢出 | ❌ | — | — | 用 `CONFIG_VMAP_STACK`（本機有開）|
| 越界**讀** | ❌ | — | — | 要用 KASAN |

> **slub_debug vs KASAN**（書上 §3.3.2 行 1990+）：slub_debug 是**事後**檢查
> （free 或下次 alloc 時才發現，抓不到越界讀、報告的時間點離犯案現場很遠）；
> KASAN 有 shadow memory，**當場**抓到、能報越界讀，代價是記憶體 ×1.125 與 2~3 倍速度損失。
> 本機 `CONFIG_KASAN is not set`。

### 產品開發時的正規開法（不需要私有 cache）

```bash
# 在 /boot/extlinux/extlinux.conf 的 APPEND 行加上：
slub_debug=FZPU              # 全部 cache 都開
slub_debug=FZPU,kmalloc-32   # 只開特定 cache（省記憶體）
slub_debug=-                 # 明確關閉
# 執行期驗證所有 cache：
echo 1 | sudo tee /sys/kernel/slab/<cache>/validate
```

---

<a name="q10"></a>
## Q10：什麼是死鎖？

### 書上定義（奔跑吧卷2 §3.4，行 2108-2114）

> 「死鎖是指兩個或多個進程因爭奪資源而造成的互相等待的現象……
> 在 Linux 內核中，常見的死鎖有：**遞歸死鎖**、**AB-BA 死鎖**。」

死鎖的四個必要條件（Coffman 條件）：互斥、持有並等待、不可剝奪、循環等待。
**內核鎖天生滿足前三個，所以防死鎖 = 破壞「循環等待」= 全域統一加鎖順序。**

### 本機的殘酷現實：沒有 lockdep，死鎖不會有人喊

```bash
ssh radxa@192.168.68.58 'zcat /proc/config.gz | grep -E "PROVE_LOCKING|DEBUG_LOCKDEP|DETECT_HUNG_TASK|SOFTLOCKUP|DEBUG_SPINLOCK|DEBUG_MUTEXES"'
```
```
# CONFIG_PROVE_LOCKING is not set      ← 書上那份漂亮的 lockdep 報告，本機印不出來
# CONFIG_DETECT_HUNG_TASK is not set   ← 卡住 120 秒也不會有 "blocked for more than 120 seconds"
# CONFIG_SOFTLOCKUP_DETECTOR is not set
# CONFIG_DEBUG_MUTEXES is not set
CONFIG_DEBUG_SPINLOCK=y                ← 只剩這個
ls /proc/lockdep* → 不存在
```

所以 [`experiments/dl_lab.c`](./experiments/dl_lab.c) 改用**可以自我解除**的方式，
把死鎖的「現象」量出來。

### 遞歸死鎖 ①：自旋鎖（`mode=0`，安全版）

```bash
sudo insmod dl_lab.ko mode=0
```
```
dl_lab: [AA/spinlock] 先拿一次 lock_s
dl_lab: 拿到後 raw_lock.owner=ffff000105cd5d00(current=ffff000105cd5d00) owner_cpu=4(this=4)
dl_lab: [AA/spinlock] 500ms 內 trylock 失敗 92897122 次 —— 這就是遞歸死鎖：自己等自己放手
```

**92,897,122 次**（500 ms 內每 5.4 ns 一次）—— 這就是自旋鎖死鎖的本質：
CPU 全速空轉、preemption 關著、誰也救不了它。
（這個數字每次跑會不一樣，取決於落在哪顆核與當下頻率：
重跑一次落在 cpu6 是 80,059,030 次，量級相同。）

`owner == current && owner_cpu == this_cpu` 這兩個欄位（`CONFIG_DEBUG_SPINLOCK=y` 才有）
正是 `kernel/locking/spinlock_debug.c:84-88` 用來判斷遞歸的條件：

```c
static inline void debug_spin_lock_before(raw_spinlock_t *lock)   /* :80 */
{
	SPIN_BUG_ON(READ_ONCE(lock->magic) != SPINLOCK_MAGIC, lock, "bad magic");
	SPIN_BUG_ON(READ_ONCE(lock->owner) == current, lock, "recursion");       /* :86 */
	SPIN_BUG_ON(READ_ONCE(lock->owner_cpu) == raw_smp_processor_id(),
						lock, "cpu recursion");         /* :87 */
}
void do_raw_spin_lock(raw_spinlock_t *lock)                       /* :112 */
{
	debug_spin_lock_before(lock);     /* 先印 BUG: spinlock recursion on CPU#n */
	arch_spin_lock(&lock->raw_lock);  /* 然後照樣卡死在這裡，永不返回 */
	mmiowb_spin_lock();
	debug_spin_lock_after(lock);
}
```

> ⚠️ 這也是為什麼實驗用 `spin_trylock` 而不是真的 `spin_lock` 第二次：
> 真的做下去，那顆 CPU 就再也回不來了（本機連 softlockup detector 都沒開，
> 連錯誤訊息都不會有，只能斷電）。

### 遞歸死鎖 ②：互斥鎖（`mode=1`，真的睡死）

```bash
sudo insmod dl_lab.ko mode=1 escape_ms=4000     # 4 秒後用 SIGKILL 解救
```
```
dl_lab: [AA/mutex] 第一次 mutex_lock(&mutex_a) 成功，owner=ffff00010adf8000
dl_lab: [AA/mutex] 再鎖一次 —— 從此進 TASK_KILLABLE，等自己
   （此時 ps 顯示： dl_aa  S  ；/proc/<pid>/stack：
       [<0>] aa_mutex_fn+0x80/0x170 [dl_lab]
       [<0>] kthread+0xc0/0xd0
       [<0>] ret_from_fork+0x10/0x20  ）
dl_lab: [watchdog] 4000 ms 到，發 SIGKILL 解除死鎖
dl_lab: [AA/mutex] 被喚醒，ret=0，睡了 4172346 us
dl_lab: [AA/mutex] signal_pending=1 fatal=1
dl_lab: [AA/mutex] mutex_a.owner 原始值 = 0xffff00010adf8000
```

自旋鎖 vs 互斥鎖的差別一目了然：**自旋鎖燒 CPU，互斥鎖靜靜地睡**（`S`/`D` 狀態、
CPU 使用率 0）。在沒有 hung_task detector 的系統上，後者**完全無聲無息**。

**意外收穫（很值得講的細節）**：`mutex_lock_interruptible()` 被 SIGKILL 叫醒後
**回傳 0（成功），而不是 -EINTR**。追進板上的原始碼
`kernel/locking/mutex.c:659-690`：

```c
		if (__mutex_trylock(lock))
			goto acquired;                          /* :659-660 */
		if (signal_pending_state(state, current)) {     /* :667 */
			ret = -EINTR;
			goto err;
		}
		...
		raw_spin_unlock(&lock->wait_lock);
		schedule_preempt_disabled();            /* ← 在這裡被 signal 叫醒 */
		first = __mutex_waiter_is_first(lock, &waiter);
		set_current_state(state);
		if (__mutex_trylock_or_handoff(lock, first))    /* :689 */
			break;                          /* ← 先 trylock，才輪到下一圈檢查 signal */
```

而 `__mutex_trylock_common()`（`mutex.c:107-130`）在 handoff 路徑裡有這麼一句：

```c
		if (atomic_long_try_cmpxchg_acquire(&lock->owner, &owner, task | flags)) {
			if (task == curr)
				return NULL;    /* NULL = 「拿到了」——但 owner 本來就是我自己！ */
```

也就是說：**遞歸自鎖的行程被喚醒後，會「假裝」成功拿到鎖**。
沒有 `CONFIG_DEBUG_MUTEXES`/lockdep 的話，這種錯誤不但不會被抓到，
還會安靜地讓鎖的狀態出錯（接下來的 `mutex_unlock()` 就是一次多餘的解鎖）。
—— 這正是文件裡寫死「**mutex 絕對不可以遞歸取得**」的原因。

（對照：`mode=2` 的 AB-BA 情境下，因為 owner 是**別人**，同一段程式碼就正確地回傳
`-EINTR`，見 Q11。）

---

<a name="q11"></a>
## Q11：常見的死鎖有哪幾種？

### 書上兩種（§3.4 行 2112-2114）

> 「**遞歸死鎖**：如在中斷等延遲操作中使用了鎖，和外面的鎖構成了遞歸死鎖。
> **AB-BA 死鎖**：多個鎖因處理不當而引發死鎖，多個內核路徑上的鎖處理順序不一致也會導致死鎖。」

### 本機重現：AB-BA（`mode=2`，真阻塞）

```bash
sudo insmod dl_lab.ko mode=2 escape_ms=5000
```
```
dl_lab: [A->B] 拿到第一把鎖 mutex_a
dl_lab: [B->A] 拿到第一把鎖 mutex_b
dl_lab: [A->B] 現在要拿第二把鎖 mutex_b
dl_lab: [B->A] 現在要拿第二把鎖 mutex_a
   （此時兩條 kthread 都掛在同一行）
     dl_ab: [<0>] abba_fn+0x174/0x23c [dl_lab] → kthread+0xc0 → ret_from_fork+0x10
     dl_ba: [<0>] abba_fn+0x174/0x23c [dl_lab] → kthread+0xc0 → ret_from_fork+0x10
dl_lab: [watchdog] 5000 ms 到，發 SIGKILL 解除死鎖
dl_lab: [A->B] 第二把鎖 ret=-4，阻塞了 4773 ms (靠 SIGKILL 逃出來)
dl_lab: [B->A] 第二把鎖 ret=0，阻塞了 4773 ms
```

`mode=3` 是同一個場景的 trylock 版本（完全安全，可以重複跑）：

```
dl_lab: [A->B] trylock 連續失敗 113 次 / 3023 ms —— 循環等待成立
dl_lab: [B->A] trylock 連續失敗 113 次 / 3020 ms —— 循環等待成立
```

**兩邊在整個窗口內互相搶不到 = 循環等待的直接量測。**

### 本機重現：書上第二個例子（`mode=4`）—— mutex vs `cancel_delayed_work_sync()`

書上（§3.4 行 2296-2461）那個「從實際項目中抽取出來」的死鎖，本機完整重現：

```
CPU0（kthread）                        CPU1（kworker）
--------------------------------------------------------------
mutex_lock(&mutex_a);
schedule_delayed_work(&delay_task);
                                       book_worker() 被叫起來
                                       mutex_lock(&mutex_a);  ← 拿不到，等 CPU0
cancel_delayed_work_sync(&delay_task);
  → flush_work() 等 worker 跑完        ← 而 worker 在等 CPU0 的鎖
              ***  DEADLOCK  ***
```

```bash
sudo insmod dl_lab.ko mode=4 escape_ms=4000
```
```
dl_lab: [book_thread] 先拿 mutex_a
dl_lab: [worker] 進來了，要拿 mutex_a
dl_lab: [book_thread] 再呼叫 cancel_delayed_work_sync() —— 會等 worker 跑完，而 worker 正在等 mutex_a

--- 死鎖進行中：ps 與 /proc/<pid>/stack ---
  16054 D    dl_book                            ← D 狀態（不可中斷睡眠）
  [<0>] __flush_work.isra.0+0x144/0x1a8
  [<0>] __cancel_work_timer+0x100/0x184
  [<0>] cancel_delayed_work_sync+0x18/0x20
  [<0>] book_thread+0x80/0xd8 [dl_lab]

dl_lab: [worker] 等 mutex_a 等了 4003 ms 還拿不到 —— 放棄
dl_lab: [book_thread] cancel_delayed_work_sync() 卡了 3746 ms 才返回
```

`/proc/<pid>/stack` 抓到的三行 **`__flush_work → __cancel_work_timer →
cancel_delayed_work_sync`**，和書上 lockdep 報告裡的
`flush_work+0x48 / __cancel_work_timer+0xe4 / cancel_delayed_work_sync+0x1c`
是同一條路徑 —— **沒有 lockdep 也能用 `/proc/<pid>/stack` 定位死鎖**，
這是本機環境下最實用的一招。

### 內核常見死鎖分類（完整版，面試用）

| # | 類型 | 典型場景 | 本機能不能自動偵測 |
|---|---|---|---|
| 1 | **AA / 遞歸死鎖** | 同一條路徑重複取同一把鎖；函式互相呼叫都各自加鎖 | spinlock：`CONFIG_DEBUG_SPINLOCK` 會印 `spinlock recursion`（然後照樣卡死）；mutex：**完全偵測不到** |
| 2 | **AB-BA** | 兩條路徑加鎖順序相反 | ❌（需 lockdep） |
| 3 | **中斷上下文 vs 行程上下文** | 行程用 `spin_lock()`，中斷處理常式用同一把 → 中斷打斷持鎖者，同 CPU 上自我死鎖。正解：`spin_lock_irqsave()` | ❌（需 lockdep 的 irq-safe/irq-unsafe 狀態機） |
| 4 | **軟中斷 / tasklet vs 行程** | 少了 `spin_lock_bh()` | ❌ |
| 5 | **同步等待自己** | 本文 `mode=4`：持鎖時呼叫 `flush_work()`/`cancel_*_sync()`/`flush_workqueue()`/`del_timer_sync()` | ❌（可用 `/proc/<pid>/stack` 人工判讀） |
| 6 | **持鎖睡眠** | 拿著 spinlock 呼叫 `kmalloc(GFP_KERNEL)`、`copy_to_user()`、`msleep()` | `CONFIG_DEBUG_ATOMIC_SLEEP`（本機**未開**） |
| 7 | **讀寫鎖遞歸** | 持有 read lock 時再取 read lock，中間夾一個 writer → writer 優先造成死鎖 | ❌ |
| 8 | **rt_mutex / 優先權反轉** | RT 任務等低優先權任務持有的鎖 | PI mutex 可緩解 |

### 防死鎖的工程作法

1. **固定加鎖順序**（破壞循環等待）——最有效，例如「永遠先 inode 後 page」。
2. **縮小臨界區**，絕不在持鎖時呼叫可能阻塞/同步等待的 API。
3. **開發階段一定要開 lockdep**：`CONFIG_PROVE_LOCKING=y` +
   `CONFIG_DEBUG_LOCKDEP=y` + `CONFIG_LOCK_STAT=y`（書上 §3.4 行 2156-2159）。
   lockdep 的價值在於**死鎖沒發生也能提前警告**（它驗的是鎖依賴圖，不是實際的卡死）。
4. 產品內核至少要開 **`CONFIG_DETECT_HUNG_TASK`**（本機沒開），
   這樣 D 狀態超過 120 秒會自動印堆疊。
5. 現場救急三招（本機驗證可用）：
   ```bash
   cat /proc/<pid>/stack                    # 卡在哪一行
   ps -eo pid,stat,wchan:32,comm | grep D   # 誰在 D 狀態
   echo w | sudo tee /proc/sysrq-trigger    # 一次列出所有阻塞任務（CONFIG_MAGIC_SYSRQ=y）
   echo l | sudo tee /proc/sysrq-trigger    # 所有 CPU 的堆疊（抓自旋鎖死鎖）
   ```

---

<a name="q12"></a>
## Q12：什麼是 printk 輸出等級？printk 包含哪些輸出等級？

### 書上（奔跑吧卷2 §3.5.1，行 2464-2514）

8 個等級，`KERN_EMERG`(0) 最高、`KERN_DEBUG`(7) 最低；
`CONFIG_MESSAGE_LOGLEVEL_DEFAULT` 決定沒帶等級的訊息算第幾級；
`/proc/sys/kernel/printk` 四個數字分別是
**控制台等級 / 默認消息等級 / 最低等級 / 默認控制台等級**。

### 本機驗證

```bash
ssh radxa@192.168.68.58 'cat /proc/sys/kernel/printk'
```
```
4	4	1	7
│   │   │   └─ default_console_loglevel  = CONFIG_CONSOLE_LOGLEVEL_DEFAULT = 7
│   │   └───── minimum_console_loglevel  = 1
│   └───────── default_message_loglevel  = CONFIG_MESSAGE_LOGLEVEL_DEFAULT = 4
└───────────── console_loglevel = 4   ← 只有 <0..3（EMERG~ERR）會送到 console
```

本機是 4 而不是書上的 7，因為 cmdline 有 `quiet loglevel=4`
（`CONFIG_CONSOLE_LOGLEVEL_QUIET=4`）。

[`experiments/printk_lab.c`](./experiments/printk_lab.c) 把 8 個等級各印一次：

```bash
sudo insmod printk_lab.ko && sudo dmesg -x | grep printk_lab
```
```
kern  :emerg : printk_lab: <0> KERN_EMERG   系統不可用
kern  :alert : printk_lab: <1> KERN_ALERT   必須立刻處理
kern  :crit  : printk_lab: <2> KERN_CRIT    臨界狀況
kern  :err   : printk_lab: <3> KERN_ERR     錯誤
kern  :warn  : printk_lab: <4> KERN_WARNING 警告
kern  :notice: printk_lab: <5> KERN_NOTICE  正常但重要
kern  :info  : printk_lab: <6> KERN_INFO    提示訊息
kern  :debug : printk_lab: <7> KERN_DEBUG   除錯訊息
kern  :info  : printk_lab: console_loglevel 目前 = 4
kern  :info  : printk_lab: default_message_loglevel = 4, minimum_console_loglevel = 1
```

`dmesg -r` 看得到原始的等級前綴（`KERN_EMERG` 其實是 `"\001" "0"`，
`include/linux/kern_levels.h:5-15`）：

```
<0>[ 2361.300947] printk_lab: <0> KERN_EMERG   系統不可用
<1>[ 2361.300970] printk_lab: <1> KERN_ALERT   必須立刻處理
...
<7>[ 2361.300986] printk_lab: <7> KERN_DEBUG   除錯訊息
```

**關鍵區別**：`console_loglevel=4` 只影響**送不送到 console（串口/tty）**，
所有訊息一律進 ring buffer，`dmesg` 全都看得到。上面 8 條在 `dmesg` 裡都在，
但只有 `<0>~<3>` 會出現在串口上。

```bash
# 執行期調整（書上行 2510）
echo 8 | sudo tee /proc/sys/kernel/printk        # 全開
sudo dmesg -n 1                                  # 只留 EMERG
```

### 8 個等級速查

| 巨集 | 值 | pr_ 版本 | 語意 | 典型用途 |
|---|---|---|---|---|
| `KERN_EMERG` | 0 | `pr_emerg` | 系統不可用 | panic 前 |
| `KERN_ALERT` | 1 | `pr_alert` | 必須立刻處理 | 資料損毀 |
| `KERN_CRIT` | 2 | `pr_crit` | 臨界狀況 | 硬體/驅動嚴重失敗 |
| `KERN_ERR` | 3 | `pr_err` | 錯誤 | 驅動 probe 失敗 |
| `KERN_WARNING` | 4 | `pr_warn` | 警告 | 可恢復的異常 |
| `KERN_NOTICE` | 5 | `pr_notice` | 正常但重要 | 安全相關 |
| `KERN_INFO` | 6 | `pr_info` | 提示 | 版本、設定 |
| `KERN_DEBUG` | 7 | `pr_debug` | 除錯 | **在 `CONFIG_DYNAMIC_DEBUG` 下變成動態的，見 Q13** |
| `KERN_CONT` | `"\001c"` | `pr_cont` | 續行 | 只在早期 boot 安全 |

### 本機實測的三個 6.1 新增細節（書上沒有）

**① `%p` 預設會被雜湊**（4.15, commit `ad67b74d2469`）：

```
printk_lab: 指標三種印法：%p=00000000577ed993（雜湊過）
                          %px=ffff80000111e400（真位址）
                          %pS=printk_lab_init+0x0/0x1000 [printk_lab]
```

要看真位址得用 `%px`，或 cmdline 加 `no_hash_pointers`。
書上表 3.2 裡的 **`%pf`/`%pF` 在 5.5 已被刪除**（commit `9af7706492f9`），
一律用 `%pS`（帶偏移）/ `%ps`（不帶）。

**② `print_hex_dump()`**（書上行 2558 提到）：

```
printk_lab: 00000000: 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f  ................
printk_lab: 00000010: 10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f  ................
```

**③ `dump_stack()`** 印出來的堆疊等級是 `KERN_WARNING`（不是 INFO），
本機實測：

```
kern  :warn  : printk_lab_init+0x190/0x1000 [printk_lab]
```

> 小坑：`dmesg` 會把 UTF-8 的全形括號等非 ASCII 位元組跳脫成 `\xbc\x88`，
> printk 訊息裡盡量用 ASCII 標點。

---

<a name="q13"></a>
## Q13：如何使用內核的動態輸出技術？

### 書上（奔跑吧卷2 §3.5.2，行 2561-2634）

打開 `CONFIG_DYNAMIC_DEBUG`，掛 debugfs，用
`/sys/kernel/debug/dynamic_debug/control` 逐條開關 `pr_debug()`/`dev_dbg()`，
選擇器有 `file` / `module` / `func`，旗標有 `p`(打開) `f`(函式名) `l`(行號) `m`(模組名) `t`(執行緒 ID)。

### 本機驗證

```bash
ssh radxa@192.168.68.58 'sudo wc -l /proc/dynamic_debug/control'
```
```
6217 /proc/dynamic_debug/control      ← 本機共 6217 條可動態開關的輸出語句
```

> **6.1 的新東西**：除了 debugfs 版本，還有 **`/proc/dynamic_debug/control`**
> （5.19, commit `b78b0961b78e`），不必掛 debugfs 就能用。兩者內容相同。

[`experiments/dyndbg_lab.c`](./experiments/dyndbg_lab.c) 裡有 5 條 `pr_debug()`，
分佈在 3 個函式裡。載入後**一條都不會印**：

```bash
sudo insmod dyndbg_lab.ko && sudo grep dyndbg_lab /proc/dynamic_debug/control
```
```
/home/radxa/exp/ch12/dyndbg_lab.c:26 [dyndbg_lab]dyndbg_one   =_ "dyndbg_one: 第 1 條，v=%d\n"
/home/radxa/exp/ch12/dyndbg_lab.c:27 [dyndbg_lab]dyndbg_one   =_ "dyndbg_one: 第 2 條，v*2=%d\n"
/home/radxa/exp/ch12/dyndbg_lab.c:32 [dyndbg_lab]dyndbg_two   =_ "dyndbg_two: 第 3 條，v=%d\n"
/home/radxa/exp/ch12/dyndbg_lab.c:33 [dyndbg_lab]dyndbg_two   =_ "dyndbg_two: 第 4 條，v=%d\n"
/home/radxa/exp/ch12/dyndbg_lab.c:38 [dyndbg_lab]dyndbg_three =_ "dyndbg_three: 第 5 條，沒有參數\n"
   格式：檔案:行號 [模組]函式 =旗標 "格式字串"   （`=_` 表示全關）
```

#### ① 用 `func` 選擇器 + 五個旗標

```bash
echo 'func dyndbg_two +pflmt' | sudo tee /proc/dynamic_debug/control
```
```
dyndbg_lab.c:32 [dyndbg_lab]dyndbg_two =pmflt "dyndbg_two: 第 3 條，v=%d\n"
dyndbg_lab.c:33 [dyndbg_lab]dyndbg_two =pmflt "dyndbg_two: 第 4 條，v=%d\n"
                                       ↑ 只有這兩條被打開
```
觸發後的輸出（`t`=執行緒 ID、`m`=模組名、`f`=函式名、`l`=行號 全都帶上了）：
```
[13207] dyndbg_lab:dyndbg_two:32: dyndbg_two: 第 3 條，v=2
[13207] dyndbg_lab:dyndbg_two:33: dyndbg_two: 第 4 條，v=2
  ↑tid   ↑module   ↑func    ↑line
```

#### ② 用 `module` 選擇器全開，再用 `file + line` 關掉單獨一條

```bash
echo 'module dyndbg_lab +p'        | sudo tee /proc/dynamic_debug/control
echo 'file dyndbg_lab.c line 26 -p'| sudo tee /proc/dynamic_debug/control
```
```
dyndbg_lab.c:26 [dyndbg_lab]dyndbg_one   =_    ← 精準關掉這一條
dyndbg_lab.c:27 [dyndbg_lab]dyndbg_one   =p
dyndbg_lab.c:32 [dyndbg_lab]dyndbg_two   =p
dyndbg_lab.c:33 [dyndbg_lab]dyndbg_two   =p
dyndbg_lab.c:38 [dyndbg_lab]dyndbg_three =p
--- 實際輸出：第 1 條果然不見了 ---
dyndbg_one: 第 2 條，v*2=2
dyndbg_two: 第 3 條，v=2
dyndbg_two: 第 4 條，v=2
dyndbg_three: 第 5 條，沒有參數
```

#### ③ 載入時就打開（模組參數）

```bash
sudo insmod dyndbg_lab.ko dyndbg=+pfl
```
```
dyndbg_one:26: dyndbg_one: 第 1 條，v=1
dyndbg_one:27: dyndbg_one: 第 2 條，v*2=2
dyndbg_two:32: dyndbg_two: 第 3 條，v=2
dyndbg_two:33: dyndbg_two: 第 4 條，v=2
dyndbg_three:38: dyndbg_three: 第 5 條，沒有參數
```

#### ④ 開機時就打開（書上行 2603-2625 的技巧，用來調試早期初始化）

```bash
# 在 /boot/extlinux/extlinux.conf 的 APPEND 加：
topology.dyndbg=+plft            # 針對某個模組/內建子系統
dyndbg="file mm/cma.c +p"        # 針對某個檔案
```

### 語法速查

```
命令格式： <選擇器> ... <旗標>
選擇器：   func <name> | file <path|glob> | module <name> | line <n>|<n-m> | format <substr>
旗標操作： +（加）  -（減）  =（設為）
旗標：     p 打開輸出   f 函式名   l 行號   m 模組名   t 執行緒ID   _ 全關
```

### 為什麼它比 printk 好用（面試講法）

| | printk | 動態輸出 |
|---|---|---|
| 粒度 | 全域一個 `console_loglevel` | **每一條語句**獨立開關 |
| 關閉時開銷 | 還是會執行 `printk()` 進 ring buffer | `pr_debug()` 被 `DYNAMIC_DEBUG_BRANCH()` 包住，**static key = 一條 NOP**（和 Q8 同一機制） |
| 要不要重編 | 改等級不用，改內容要 | 完全不用 |
| 額外資訊 | 自己寫 `__func__`/`__LINE__` | 旗標 `flmt` 自動帶上 |

補充：另外兩種傳統作法（書上行 2627-2633）——
在子系統 Makefile 加 `ccflags-y := -DDEBUG` 會讓該目錄下所有 `pr_debug()`
**變成編譯期就打開的 `printk(KERN_DEBUG)`**（失去動態能力），適合臨時 debug。

---

<a name="q14"></a>
## Q14：如何分析一個 oops 錯誤日誌？

### 書上（奔跑吧卷2 §3.5.3，行 2635-2857）

寫一個 `*(int *)0 = 0;` 的模組 → 看 oops → 用 `objdump -Sd`、
`gdb list *create_oops+0x14`、`scripts/decodecode` 三種方法定位。

### 本機的真 oops

```bash
ssh radxa@192.168.68.58 'cd ~/exp/ch12 && sudo insmod oops_m8.ko mode=1; sudo dmesg'
```

```
 1  oops_lab: init mode=1，create_oops=ffff800001286000
 2  oops_lab: 模組 .text 基底 = ffff800001286000
 3  Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000
 4  Mem abort info:
 5    ESR = 0x0000000096000044
 6    EC = 0x25: DABT (current EL), IL = 32 bits
 7    SET = 0, FnV = 0
 8    EA = 0, S1PTW = 0
 9    FSC = 0x04: level 0 translation fault
10  Data abort info:
11    ISV = 0, ISS = 0x00000044
12    CM = 0, WnR = 1
13  user pgtable: 4k pages, 48-bit VAs, pgdp=000000016eb95000
14  [0000000000000000] pgd=0000000000000000, p4d=0000000000000000
15  Internal error: Oops: 0000000096000044 [#3] SMP
16  Modules linked in: oops_m8(O+) ... [last unloaded: dyndbg_lab(O)]
17  CPU: 7 PID: 14739 Comm: insmod Tainted: G    B D W  O       6.1.115+ #1
18  Hardware name: Radxa ROCK 5B (DT)
19  pstate: 60400009 (nZCv daif +PAN -UAO -TCO -DIT -SSBS BTYPE=--)
20  pc : create_oops+0x4c/0x58 [oops_m8]
21  lr : oops_lab_init+0xa0/0x1000 [oops_m8]
22  sp : ffff80000dc2bac0
23  x29: ffff80000dc2bac0 x28: ffff800009f6a498 x27: 0000000000000008
    ...（x0~x30 全部 31 個暫存器）
24  Call trace:
25   create_oops+0x4c/0x58 [oops_m8]
26   do_one_initcall+0x88/0x1cc
27   do_init_module+0x54/0x1d8
28   load_module+0x18f8/0x1974
29   __do_sys_finit_module+0x104/0x124
30   __arm64_sys_finit_module+0x20/0x28
31   invoke_syscall+0x80/0x118
32   el0_svc_common.constprop.0+0x94/0x134
33   do_el0_svc+0x98/0xbc
34   el0_svc+0x24/0x48
35   el0t_64_sync_handler+0xa8/0x134
36   el0t_64_sync+0x174/0x178
37  Code: 0b030000 0b020021 0b010000 d2800001 (b9000020)
38  ---[ end trace 0000000000000000 ]---
```

### 逐行怎麼讀（八個步驟）

**① 第 3 行：出了什麼事** — 存取虛擬位址 0 → 空指標。
若是 `Unable to handle kernel paging request at virtual address ffff8888...`
就是野指標；`at virtual address 0000000000000010` 這種小數字通常是
「某個結構指標是 NULL，程式碼在取它的成員（offset 0x10）」。

**② 第 5-12 行：ESR 解碼** —— 這是 ARM64 特有、最值錢的一段。
`ESR = 0x96000044` 拆開來看（本機核心已經幫忙翻譯成文字了）：

```
0x96000044 = 1001 0110 0000 ... 0100 0100
  EC   = 0x25 (bit 31:26 = 100101)  Data Abort，來自同一個異常等級（核心態）
  IL   = 1                          32 位元指令
  ISV  = 0                          沒有有效的指令語法資訊
  FSC  = 0x04 (bit 5:0 低位)         level 0 translation fault（連 PGD 都沒有）
  WnR  = 1   (bit 6)                 ★ Write，是「寫」造成的
```

**同一支模組改成讀（`mode=2`）就能看出差別**：

```
  ESR = 0x0000000096000004      ← 只差在 WnR 這一位
  CM = 0, WnR = 0               ← Read
  pc : read_oops+0xc/0x14 [oops_m2]
  Code: d65f03c0 aa1e03e9 d503201f d2800000 (b9400000)   ← b9400000 = ldr w0,[x0]
```

| EC | 意義 | 常見原因 |
|---|---|---|
| 0x25 | DABT (current EL) | 核心態資料存取異常 ← 最常見 |
| 0x24 | DABT (lower EL) | 使用者態存取異常 |
| 0x21 | IABT (current EL) | **取指令**異常（跳到爛指標，`mode=4`） |
| 0x3C | BRK | `BUG()`/`WARN()` 的 `brk #0x800`（`mode=5`） |

**③ 第 15 行：`[#3]`** 是**開機以來第幾次 oops**（本機因為前面做過實驗所以是 #3）。
`SMP` 表示多核心。若設了 `panic_on_oops=1`（本機是 0），這裡就會直接 panic。

**④ 第 17 行：`Tainted: G B D W O`** ——
`G`=GPL、`B`=發生過 bad page、`D`=**之前已經 oops 過**、`W`=發生過 WARN、`O`=載入了樹外模組。
上游看到 `O`/`D` 通常就不受理 bug report 了。

**⑤ 第 20-21 行：PC 與 LR** —— **`create_oops+0x4c/0x58`
＝「在 `create_oops` 這個函式的第 0x4c 個位元組，該函式共 0x58 位元組」**（書上行 2736-2738）。
`lr` 告訴你是誰呼叫它的（`oops_lab_init+0xa0`）。

**⑥ 第 19 行：`pstate`** —— `60400009` 的 `daif` 小寫表示中斷是**開著**的；
如果印出大寫 `DAIF`，代表當時關中斷（在中斷或臨界區裡出事，難度立刻升級）。

**⑦ 第 24-36 行：Call trace** —— 本機這條非常標準：
`el0t_64_sync → el0_svc → invoke_syscall → __arm64_sys_finit_module → load_module
→ do_init_module → do_one_initcall → 出事的函式`，
一眼看出是「使用者呼叫 `insmod`，在模組 init 裡掛掉」。

**⑧ 第 37 行：`Code:`** —— PC 前 4 條 + 出錯那一條（括號內）。
`scripts/decodecode` 直接翻譯：

```bash
echo 'Code: 0b030000 0b020021 0b010000 d2800001 (b9000020)' > code.txt
ARCH=arm64 ~/disk/kernel-source/scripts/decodecode < code.txt
```
```
All code
========
   0:	0b030000 	add	w0, w0, w3
   4:	0b020021 	add	w1, w1, w2
   8:	0b010000 	add	w0, w0, w1
   c:	d2800001 	mov	x1, #0x0                   	// #0     ← 位址算成 0
  10:*	b9000020 	str	w0, [x1]		<-- trapping instruction   ← 往 0 寫
```

**這五條指令就是「有沒有原始碼都能定位」的殺手鐧**（書上行 2817-2856）。

### 三種回到原始碼的方法（本機全部實測可用）

```bash
# ① faddr2line —— 最快，直接給檔名行號（需要 .ko 帶 -g）
~/disk/kernel-source/scripts/faddr2line ./oops_m8.ko create_oops+0x4c/0x58
```
```
create_oops+0x4c/0x58:
create_oops at /home/radxa/exp/ch12/oops_m8.c:46      ← 就是 *(int *)0 = ... 那一行
```

```bash
# ② objdump -dS —— 原始碼與組語交錯，最能看清楚上下文
objdump -dS --disassemble=create_oops oops_m8.o
```
```
	volatile int a = 1, b = 2, c = 3, d = 4;
   c:	52800020 	mov	w0, #0x1
  10:	b90003e0 	str	w0, [sp]
  ...
	*(int *)0 = a + b + c + d;	/* 人為製造一個空指標寫入 */
  3c:	0b030000 	add	w0, w0, w3
  40:	0b020021 	add	w1, w1, w2
  44:	0b010000 	add	w0, w0, w1
  48:	d2800001 	mov	x1, #0x0
  4c:	b9000020 	str	w0, [x1]        ← PC = create_oops+0x4c，和 Code: 行完全對上
```

```bash
# ③ gdb（書上的方法）
gdb -batch -ex "list *create_oops+0x4c" oops_m8.o
# ④ 核心函式出事時：用 System.map / kallsyms
sudo grep -w " __slab_free" /proc/kallsyms
# ⑤ 整段日誌自動加上行號：
sudo dmesg | ~/disk/kernel-source/scripts/decode_stacktrace.sh vmlinux
```

### 其它三種「掛掉」的長相（本機實測）

**`BUG_ON(1)`（mode=5）** —— 直接 `brk`，訊息裡**自帶檔名行號**：

```
------------[ cut here ]------------
kernel BUG at /home/radxa/exp/ch12/oops_m5.c:61!
Internal error: Oops - BUG: 00000000f2000800 [#5] SMP
pc : bug_oops+0x18/0x20 [oops_m5]
Code: d0000000 b9405800 7100141f 54000041 (d4210000)
                                            ↑ brk #0x800
```
`ESR = 0xf2000800`：EC=0x3C（BRK）、imm=0x800。
ARM64 的 `BUG()` 不是 `panic()`（書上行 2862-2866 寫的是通用實作），
而是 `brk #0x800` + `__bug_table` 記錄檔名行號（`arch/arm64/include/asm/bug.h`），
所以才印得出 `kernel BUG at 檔案:行號`。

**`WARN_ON(1)`（mode=6）** —— 只印堆疊，**系統照跑**：

```
------------[ cut here ]------------
WARNING: CPU: 1 PID: 15096 at /home/radxa/exp/ch12/oops_m6.c:66 warn_oops+0x18/0x20 [oops_m6]
...
---[ end trace 0000000000000000 ]---
```

**oops 發生在 kthread（mode=7）** —— Call trace 的尾巴變成 `kthread → ret_from_fork`，
`Comm:` 也不再是 insmod：

```
CPU: 2 PID: 15113 Comm: oops_lab Tainted: G    B D W  O
pc : create_oops+0xc/0x14 [oops_m7]
Call trace:
 create_oops+0xc/0x14 [oops_m7]
 kthread+0xc0/0xd0
 ret_from_fork+0x10/0x20
```

### 實驗踩到的兩個坑（很值得記）

1. **`Code: bad PC value`** —— 如果出錯指令離模組 `.text` 起點不到 16 B，
   `dump_kernel_instr()` 讀不到 PC-16 的位置，就只印這句、什麼線索都沒有。
   本機第一版 `create_oops()` 剛好被排在模組最前面，就吃到這個。
2. **oops 過的模組會卡死** —— `module_init` 裡 oops 之後，`insmod` 行程被殺，
   `do_init_module()` 永遠不會完成，模組永遠停在 `MODULE_STATE_COMING`：
   `lsmod` 看得到、`rmmod` 移不掉、同名模組也無法再載入。**只能重開機**。
   所以實驗腳本才會把 `oops_lab.c` 複製成 7 份不同名字。

### oops vs panic（面試常接著問）

| | oops | panic |
|---|---|---|
| 觸發 | 核心態存取異常、`BUG()` | `panic()`、`panic_on_oops=1`、中斷上下文的 oops |
| 後果 | **殺掉當前行程**，系統續跑（但已不可信） | 整個系統停住 |
| 本機設定 | `/proc/sys/kernel/panic_on_oops = 0` | `/proc/sys/kernel/panic = 0`（不自動重啟） |
| 產品建議 | `panic_on_oops=1` + `panic=10` + kdump，讓現場被完整保存 | |

---

## 附錄 A：實驗檔案清單

| 檔案 | 題號 | 用途 |
|---|---|---|
| [`opt_lab.c`](./experiments/opt_lab.c) + [`opt_build.sh`](./experiments/opt_build.sh) | Q1 | 四種優化等級的產出比較、`-O0` 編譯失敗實例 |
| [`pic_asm.S`](./experiments/pic_asm.S) + [`pic_demo.c`](./experiments/pic_demo.c) | Q2~Q5 | 把程式碼搬家，量 PIC / non-PIC 指令的差異 |
| [`addr_probe.c`](./experiments/addr_probe.c) | Q2、Q5、Q7 | 印出核心的加載/運行/鏈接地址與 `kimage_voffset`、`kaslr_offset` |
| [`reloc_mod.c`](./experiments/reloc_mod.c) | Q4 | 模組重定位「前 vs 後」的機器碼對照 |
| [`tp_lab.c`](./experiments/tp_lab.c) + [`tp_lab_trace.h`](./experiments/tp_lab_trace.h) | Q8 | 樹外 `TRACE_EVENT()`、static key 的 NOP↔B 改寫 |
| [`slub_lab.c`](./experiments/slub_lab.c) | Q9 | 私有 debug cache，重現五種記憶體錯誤 |
| [`dl_lab.c`](./experiments/dl_lab.c) | Q10、Q11 | 五種死鎖情境，全部可自我解除 |
| [`printk_lab.c`](./experiments/printk_lab.c) | Q12 | 8 個等級 + hex dump + dump_stack + 指標格式 |
| [`dyndbg_lab.c`](./experiments/dyndbg_lab.c) | Q13 | 5 條 `pr_debug()` 的動態開關 |
| [`oops_lab.c`](./experiments/oops_lab.c) | Q14 | 7 種掛掉方式 |
| [`ch12_run_all.sh`](./experiments/ch12_run_all.sh) | 全部 | 一鍵佈署 + 重現 |
| [`ksym.h`](./experiments/ksym.h) | — | 用 kprobe 取回 `kallsyms_lookup_name()`（沿用 ch11） |

## 附錄 B：本章用到的核心原始碼位置（板上 `~/disk/kernel-source`）

| 主題 | 檔案:行 |
|---|---|
| 編譯優化等級 | `Makefile:829-835` |
| 核心連結腳本（鏈接地址、`.rela.dyn`） | `arch/arm64/kernel/vmlinux.lds.S:154-161, 259-262` |
| 核心映像重定位 | `arch/arm64/kernel/head.S:706-793`（`__relocate_kernel`）、`796-825`（`__primary_switch`） |
| `kimage_voffset` 記錄 | `arch/arm64/kernel/head.S:433`；`arch/arm64/mm/mmu.c:55-59` |
| `kaslr_offset()` | `arch/arm64/include/asm/memory.h:201-204` |
| 模組重定位 | `arch/arm64/kernel/module.c: apply_relocate_add()` |
| U-Boot 重定位 | `u-boot/arch/arm/lib/relocate_64.S`、`u-boot/arch/arm/lib/crt0_64.S`、`u-boot/common/board_f.c:317-430` |
| 跟蹤點巨集 | `include/linux/tracepoint.h`、`include/trace/define_trace.h` |
| static key（arm64） | `arch/arm64/kernel/jump_label.c` |
| slub 檢查與訊息 | `mm/slub.c:1162, 1220, 1403, 2855, 4400`；旗標字母 `mm/slub.c:1461-1478` |
| spinlock 遞歸檢查 | `kernel/locking/spinlock_debug.c:80-89, 112-118` |
| mutex 慢路徑 | `kernel/locking/mutex.c:107-130, 596-700` |
| printk 等級定義 | `include/linux/kern_levels.h:5-15` |
| 動態輸出 | `lib/dynamic_debug.c`；控制檔 `/proc/dynamic_debug/control` |
| oops 輸出 | `arch/arm64/kernel/traps.c: die()/dump_kernel_instr()`；`arch/arm64/mm/fault.c` |
| BUG/WARN | `arch/arm64/include/asm/bug.h`（`brk #0x800` + `__bug_table`） |
| 解碼工具 | `scripts/decodecode`、`scripts/faddr2line`、`scripts/decode_stacktrace.sh` |

## 附錄 C：本章沒能在本機完成的部分（誠實記錄）

| 項目 | 原因 | 替代作法 |
|---|---|---|
| 書上的 GDB 單步調試 head.S（§3.1.3-3.1.5） | 真板子沒有 JTAG/`-s -S`，且 `CONFIG_DEBUG_INFO_NONE=y`（沒有 DWARF、板上沒有 vmlinux） | 改用 `addr_probe.ko` 把 head.S 會用到的每個位址在**執行期**印出來（Q2、Q7） |
| 核心映像重定位的非零位移 | `CONFIG_RANDOMIZE_BASE=n` 且 Image 載入位址 2 MB 對齊 → `x23=0`、`kaslr_offset()=0` | 改用**模組**展示同一套重定位機制（Q4），並用 `readelf -r vmlinux` 顯示 26 萬筆待處理項 |
| `slub_debug=UFPZ` 開機參數 | 需要改 `/boot/extlinux/extlinux.conf` 並重開機（會動到使用者的開機設定） | 用私有 `kmem_cache` 帶相同四個旗標，走**完全相同**的 `mm/slub.c` 檢查路徑（Q9），並附上正規的 cmdline 寫法 |
| lockdep 的死鎖報告 | `CONFIG_PROVE_LOCKING=n`，要重編核心 | 用可自我解除的實驗量出死鎖的**現象**（trylock 失敗次數、阻塞時間、D 狀態、`/proc/<pid>/stack`），並附上 lockdep 該開哪些選項（Q10、Q11） |
| `Freepointer corrupt` 訊息 | 打開 `SLAB_POISON` 時 freepointer 被移出物件，先撞到 poison 檢查 | 標註訊息出處 `mm/slub.c:1162/1220` 與觸發條件（Q9 ④） |
