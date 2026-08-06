# crash 工具使用指南（繁體中文）

> **這份文件是什麼**：把 `crash` 這個核心轉儲分析工具「從零到能用」的完整流程整理成一份可以照著做的筆記——
> 包含**三個前提怎麼準備**、**命令速查**、**十個實戰流程**、**ARM64 特有的坑**、以及**錯誤訊息排錯表**。
>
> **資料來源**
> - 《奔跑吧 Linux 內核》（第二版）卷2 §4.1、§4.3、§4.4、§4.12 與第 5 章
>   （`books/running-linux-kernel/running-kernel-2-txt/11_第4章_基于x86_64解决宕机难题.txt`、
>   `12_第5章_基于ARM64解决宕机难题.txt`，行號以純文字檔為準）
> - 本專案在 **Radxa ROCK 5B（RK3588、aarch64、Linux 6.1.115+）** 上的實測，
>   詳見 📝 [ch13](./ch13_x86_64_crash_debugging.md)、📝 [ch14](./ch14_arm64_crash_debugging.md)
>
> **標記約定**：
> 　📕 = 書上的範例輸出（x86_64 / CentOS 3.10 或 QEMU ARM64 / 5.0）
> 　🔧 = 本專案在 ROCK 5B 上的實測輸出
> 　⚠ = 本機實測踩到的坑

---

## 目錄

1. [crash 是什麼、什麼時候用](#s1)
2. [三個前提：vmcore、vmlinux、版本相符](#s2)
3. [準備材料 A：安裝 crash](#s3)
4. [準備材料 B：帶除錯符號的 vmlinux](#s4)
5. [準備材料 C：vmcore（設定 Kdump）](#s5)
6. [不重開機也想用：分析「活的系統」](#s6)
7. [啟動 crash：開場那 20 行怎麼讀](#s7)
8. [命令速查表](#s8)
9. [實戰流程 1：拿到 vmcore 的前五分鐘](#s9)
10. [實戰流程 2：空指標／oops 類崩潰](#s10)
11. [實戰流程 3：系統卡死、一堆 D 狀態行程](#s11)
12. [實戰流程 4：從堆疊推導區域變數與參數](#s12)
13. [實戰流程 5：誰持有鎖、誰在等鎖](#s13)
14. [實戰流程 6：一個行程被阻塞了多久](#s14)
15. [實戰流程 7：記憶體吃光了](#s15)
16. [實戰流程 8：批次化與腳本化](#s16)
17. [ARM64 專屬注意事項](#s17)
18. [本機（RK3588）現況與替代方案](#s18)
19. [錯誤訊息排錯表](#s19)
20. [附錄：延伸閱讀](#s20)

---

<a name="s1"></a>
## 1. crash 是什麼、什麼時候用

`crash` 是 Red Hat 開發的**核心轉儲分析工具**，本質上是「**gdb + 一整套懂 Linux 核心資料結構的命令**」。
它把 gdb 當函式庫用（所以 `p`、`struct`、`dis` 這些命令的語法和 gdb 很像），
再加上 `ps`、`bt`、`kmem`、`irq`、`runq`、`list`、`foreach` 這些**核心專用**命令。

**適用場景**（書上 §4.1，行 45-60）：

| 適用 | 不適用 |
|---|---|
| 系統宕機、黑屏、無回應（SSH/串列埠/鍵盤都沒反應） | **硬體錯誤造成的當機**（不能熱重啟、必須斷電） |
| oops / panic / softlockup / hardlockup / hung_task | 記憶體內容已經被破壞或掉電 |
| 死鎖、記憶體耗盡、行程被長時間阻塞 | 使用者空間程式的崩潰（那是 gdb + core dump 的工作） |
| 事後分析（post-mortem），不需要重現問題 | 需要「單步執行」的除錯（那要 kgdb/QEMU） |

**一句話**：Kdump 負責在系統死掉的瞬間把記憶體整包存下來（`vmcore`），crash 負責把那包東西**翻譯成人看得懂的東西**。

---

<a name="s2"></a>
## 2. 三個前提：vmcore、vmlinux、版本相符

啟動一次 crash 的完整命令長這樣：

```bash
crash <vmlinux> <vmcore>
```

三個前提缺一不可：

| # | 前提 | 說明 | 沒有的話 |
|---|---|---|---|
| 1 | **vmcore** | 崩潰當下的記憶體轉儲，由 Kdump 產生（`/var/crash/<日期>/vmcore`） | 只能分析活的系統（見[第 6 節](#s6)） |
| 2 | **vmlinux** | **帶 DWARF 除錯資訊**的核心映像；`/boot/vmlinuz-*` 是壓縮過、剝掉符號的，**不能用** | crash 認不得任何資料結構 |
| 3 | **版本完全相符** | crash 會逐字比對 vmlinux 裡的 `linux_banner` 與轉儲裡的版本字串 | `vmlinux and vmcore do not match!` |

> ⚠ 第 3 點比想像中嚴格：**連 `#1 SMP <編譯時間>` 都要一模一樣**。
> 本專案重建 vmlinux 時就因為少設 `KBUILD_BUILD_VERSION=1`，橫幅變成 `# SMP`（少一個 1）而被 crash 拒絕。

---

<a name="s3"></a>
## 3. 準備材料 A：安裝 crash

```bash
# Debian / Ubuntu（本專案的板子就是這個）
sudo apt-get install -y crash

# CentOS / RHEL（書上 §4.3，行 211-215）
sudo yum install -y kexec-tools crash
```

🔧 本機實測：

```console
$ sudo apt-get install -y crash
$ crash --version
crash 8.0.2
Copyright (C) 2002-2022  Red Hat, Inc.
```

順手把符號解析工具也裝上（分析模組時會用到）：

```bash
sudo apt-get install -y binutils gdb            # objdump / nm / addr2line / gdb
```

---

<a name="s4"></a>
## 4. 準備材料 B：帶除錯符號的 vmlinux

### 4.1 三種取得方式

| 情境 | 做法 |
|---|---|
| **發行版核心**（Ubuntu/Debian/CentOS） | 裝 debuginfo 套件：<br>CentOS：`yum install kernel-debuginfo-$(uname -r)` → `/usr/lib/debug/lib/modules/<ver>/vmlinux`（書上 §4.3，行 270-287）<br>Debian/Ubuntu：`apt install linux-image-$(uname -r)-dbgsym`（要先加 ddebs 來源） |
| **自己編的核心** | 編譯時打開 `CONFIG_DEBUG_INFO_DWARF4=y`（或 `DWARF5`），vmlinux 就在原始碼樹根目錄，**不要 strip** |
| **廠商給的核心，但沒有 vmlinux** | 只要**原始碼、組態、編譯器版本都能還原**，就可以自己重建一個「位址完全相同」的 vmlinux（見下） |

### 4.2 🔧 本專案的做法：重建一個和跑著的核心完全相符的 vmlinux

板子上的 6.1.115+ 是**在板子上自己編的**（`/proc/version` 顯示 `radxa@rock-5b`、gcc 12.2.0），
原始碼樹還在，而且 `CONFIG_RELOCATABLE=y` 但**沒有 KASLR** → 只要組態與編譯器一致，符號位址就會一致。

完整腳本：[`experiments/build_vmlinux.sh`](./experiments/build_vmlinux.sh)

```bash
cp -a ~/disk/kernel-source ~/kbuild-src        # 複製一份，不要動 /lib/modules/<ver>/build 那棵
cd ~/kbuild-src
printf '+' > .scmversion                       # 版本尾巴那個 "+"
zcat /proc/config.gz > .config                 # ★ 用「正在跑的核心」的組態當基礎
./scripts/config -d DEBUG_INFO_NONE -e DEBUG_INFO_DWARF4 \
                 -d DEBUG_INFO_REDUCED -d DEBUG_INFO_SPLIT -d DEBUG_INFO_BTF
make olddefconfig
KBUILD_BUILD_TIMESTAMP='Mon Apr 27 08:30:35 UTC 2026' \
KBUILD_BUILD_USER=radxa KBUILD_BUILD_HOST=rock-5b KBUILD_BUILD_VERSION=1 \
        make -j8 vmlinux                       # 8 核約 25 分鐘
```

**做完一定要驗證這兩件事**：

```console
$ strings vmlinux | grep -m1 "^Linux version"
Linux version 6.1.115+ (radxa@rock-5b) (gcc (Debian 12.2.0-14+deb12u1) 12.2.0, …) #1 SMP Mon Apr 27 08:30:35 UTC 2026
$ cat /proc/version
Linux version 6.1.115+ (radxa@rock-5b) (gcc (Debian 12.2.0-14+deb12u1) 12.2.0, …) #1 SMP Mon Apr 27 08:30:35 UTC 2026
                                                                                  ↑ 必須一字不差

$ nm vmlinux | grep -w _stext                  # 符號位址要和執行中的 kallsyms 相同
ffff800008010000 T _stext
$ sudo grep -w " _stext$" /proc/kallsyms
ffff800008010000 T _stext                      ✓
```

> 💡 就算最後 crash 起不來，這顆 vmlinux 也**非常值得做**：
> `objdump -d vmlinux`（= crash 的 `dis`）、`gdb vmlinux`、`faddr2line` 全都靠它。

---

<a name="s5"></a>
## 5. 準備材料 C：vmcore（設定 Kdump）

### 5.1 原理一句話

Kexec 讓「崩潰中的核心」**不經過 BIOS/bootloader** 直接跳進一個預先載入好的**捕獲核心**，
於是第一個核心的記憶體原封不動地變成第二個核心眼中的 `/proc/vmcore`，
再由 `makedumpfile` 存成檔案。（書上 §4.1，行 32-60；詳細六步驟見 📝 [ch13 Q1](./ch13_x86_64_crash_debugging.md#q1)）

### 5.2 三件事缺一不可

**（1）核心組態**

```
CONFIG_KEXEC=y                 # 或 CONFIG_KEXEC_FILE=y
CONFIG_CRASH_DUMP=y
CONFIG_DEBUG_INFO_DWARF4=y     # 要有 vmlinux 才分析得動
CONFIG_PROC_VMCORE=y
# 建議一起開：
CONFIG_MAGIC_SYSRQ=y           # 用 sysrq-c 手動觸發測試
CONFIG_SOFTLOCKUP_DETECTOR=y   CONFIG_HARDLOCKUP_DETECTOR=y   CONFIG_DETECT_HUNG_TASK=y
CONFIG_PROC_KCORE=y            # 讓 crash 也能分析活的系統
```

**（2）開機參數保留記憶體**

```bash
# x86_64（書上 §4.3，行 217-224）
GRUB_CMDLINE_LINUX="... crashkernel=512M"
sudo grub2-mkconfig -o /boot/grub2/grub.cfg && sudo reboot

# ARM64 / 嵌入式（extlinux、uEnv.txt、U-Boot bootargs 依板子而異）
... crashkernel=256M
```

確認保留成功：

```bash
cat /sys/kernel/kexec_crash_size      # 不是 0 就對了
grep -i crash /proc/iomem             # 會看到 "Crash kernel" 這一段
```

**（3）使用者空間服務**

```bash
# CentOS
sudo systemctl enable --now kdump.service && systemctl status kdump.service   # 要看到 active
# Debian/Ubuntu
sudo apt-get install -y kdump-tools && systemctl status kdump-tools
cat /sys/kernel/kexec_crash_loaded    # 要變成 1
```

### 5.3 測試與產物

```bash
sudo sh -c 'echo 1 > /proc/sys/kernel/sysrq; echo c > /proc/sysrq-trigger'   # 立刻當機並轉儲
```

重開機後：

```console
# CentOS：/var/crash/<IP>-<日期>/
127.0.0.1-2019-03-02-21:41:33/vmcore
127.0.0.1-2019-03-02-21:41:33/vmcore-dmesg.txt      ← 崩潰當下的 dmesg，先看這個
# Debian：/var/crash/<日期>/dump.<日期>
```

想在「真的死機」時也能抓到，記得打開對應的 panic 開關：

```bash
echo 1 > /proc/sys/kernel/panic_on_oops
echo 1 > /proc/sys/kernel/softlockup_panic
echo 1 > /proc/sys/kernel/hardlockup_panic
echo 1 > /proc/sys/kernel/hung_task_panic
echo 30 > /proc/sys/kernel/hung_task_timeout_secs
```

---

<a name="s6"></a>
## 6. 不重開機也想用：分析「活的系統」

crash 也能分析正在跑的系統（`crash <vmlinux>`，不給 vmcore）。它按順序找記憶體來源：

| 來源 | 需要 | 說明 |
|---|---|---|
| `/dev/crash` | crash 官方的 memory driver（要自己編） | 最可靠 |
| `/dev/mem` | `CONFIG_DEVMEM=y` 且 **`CONFIG_STRICT_DEVMEM=n`** | 大多數發行版都開了 STRICT，會被擋 |
| `/proc/kcore` | `CONFIG_PROC_KCORE=y` | **ARM64 上幾乎是必要的**（見下） |

編 memory driver：

```bash
curl -O https://raw.githubusercontent.com/crash-utility/crash/master/memory_driver/crash.c
curl -O https://raw.githubusercontent.com/crash-utility/crash/master/memory_driver/Makefile
make && sudo insmod crash.ko && ls -l /dev/crash
```

🔧 本機實測（ROCK 5B）：driver 編得起來、`/dev/crash` 出得來、`dd` 讀實體記憶體也正確：

```console
$ sudo dd if=/dev/crash bs=1 skip=$((0x148e0c0)) count=16 2>/dev/null | od -A x -t x1
000000 49 4b 43 46 47 5f 53 54 1f 8b 08 00 00 00 00 00      ← "IKCFG_ST" = kernel_config_data
```

⚠ **但 crash 還是起不來**：本機 `# CONFIG_PROC_KCORE is not set`，
crash 8.0.2 在「沒有 vmcore、也沒有 `/proc/kcore`」時只能**猜** VA_BITS，
而 5.11 之後 ARM64 的虛擬位址佈局翻轉了（核心映像在 `0xffff8000…`、線性映射在 `0xffff0000…`），
它猜成 47（實際 48）→ 所有位址轉換都錯 → `vmlinux and /dev/crash do not match!`

**結論：ARM64 想分析活系統，`CONFIG_PROC_KCORE=y` 幾乎是必備**（crash 會從 kcore 的 PT_NOTE 讀到 VMCOREINFO）；
否則就走正規的 Kdump 路線。完整分析見 📝 [ch14 附錄 A](./ch14_arm64_crash_debugging.md#appendix-a)。

---

<a name="s7"></a>
## 7. 啟動 crash：開場那 20 行怎麼讀

```bash
crash /usr/lib/debug/lib/modules/$(uname -r)/vmlinux /var/crash/2019-03-20-23:01:54/vmcore
```

📕 開場輸出（書上 §4.3，行 289-309）：

```
      KERNEL: /usr/lib/debug/lib/modules/3.10.0-957.1.3.el7.x86_64/vmlinux
    DUMPFILE: /var/crash/127.0.0.1-2019-03-20-23:01:54/vmcore  [PARTIAL DUMP]
        CPUS: 4
        DATE: Wed Mar 20 23:01:49 2019
      UPTIME: 01:49:54
LOAD AVERAGE: 0.69, 0.28, 0.14
       TASKS: 418
    NODENAME: localhost.localdomain
     RELEASE: 3.10.0-957.1.3.el7.x86_64
     VERSION: #1 SMP Thu Nov 29 14:49:43 UTC 2018
     MACHINE: x86_64  (2496 Mhz)
      MEMORY: 2 GB
       PANIC: "SysRq : Trigger a crash"
         PID: 15207
     COMMAND: "bash"
        TASK: ffff8fc655e5b0c0  [THREAD_INFO: ffff8fc643a34000]
         CPU: 0
       STATE: TASK_RUNNING (SYSRQ)
crash>
```

**這 20 行就是第一份線索**，要看的順序：

| 欄位 | 怎麼用 |
|---|---|
| `PANIC:` | **第一個要看的**。是 `SysRq`（人為）、`NULL pointer dereference`（空指標）、`softlockup: hung tasks`、`hung_task: blocked tasks`（阻塞）還是 `Oops:`？不同起因走不同流程（見第 9~11 節） |
| `PID` / `COMMAND` / `TASK` | 崩潰當下的「當前行程」。注意：**panic 的行程不一定是兇手**——`khungtaskd` panic 時，兇手是別的 D 狀態行程 |
| `CPU` | 出事的 CPU，之後 `bt -a` 可以看每顆 CPU 在幹嘛 |
| `UPTIME` | 開機多久出事，配合 `dmesg` 時間戳定位 |
| `[PARTIAL DUMP]` | makedumpfile 有壓縮／過濾（通常濾掉 free page 與 user page），**代表有些頁面讀不到是正常的** |
| `LOAD AVERAGE` / `TASKS` | 負載爆高 + 大量 task → 往資源耗盡方向查 |

---

<a name="s8"></a>
## 8. 命令速查表

crash 大約有 50 個子命令（書上 §4.4，行 313-360 列出全部）。按**用途**分類的常用清單：

### 8.1 先看全局

| 命令 | 用途 | 常用選項 |
|---|---|---|
| `help` / `help <cmd>` | 線上說明（**最重要的命令**） | — |
| `sys` | 重印開場那段系統資訊 | `sys -c` 看 cmdline、`sys config` 看核心組態 |
| `log` | 崩潰當下的 dmesg | `log -T`（顯示時間戳） |
| `mach` | CPU / 記憶體 / 架構資訊 | — |
| `mount` / `files` | 掛載點 / 開啟的檔案 | `files <pid>` |

### 8.2 行程與堆疊

| 命令 | 用途 | 常用選項 |
|---|---|---|
| `ps` | 行程列表 | `ps \| grep UN`（D 狀態）、`ps -u`（只看使用者行程 + RSS）、`ps -k`（只看核心執行緒）、`ps -m`（顯示時間） |
| `bt` | **backtrace，最常用**（書上 §4.4-2） | `bt <pid>`、`bt -f`（**印每個框架的原始堆疊內容**）、`bt -l`（顯示行號）、`bt -a`（每顆 CPU 的當前行程）、`bt -t`（只列 text 符號） |
| `set` | 切換「當前行程上下文」 | `set <pid>`，之後 `bt`、`task` 都以它為準 |
| `task` | 印 `task_struct` | `task -R sched_info,comm <pid>`（只印指定成員） |
| `runq` | 各 CPU 的執行佇列 | `runq -t`（**印每顆 CPU 的時間戳**，算阻塞時間用） |
| `foreach` | 對一批行程重複下命令 | `foreach UN bt`、`foreach RU ps`、`foreach bash bt` |
| `waitq` | 印等待佇列上的行程 | `waitq <addr>` |

### 8.3 記憶體與資料結構

| 命令 | 用途 | 常用選項 |
|---|---|---|
| `rd` | 讀記憶體（書上 §4.4-6） | `rd <addr> <n>`（連讀 n 個）、`-p`（實體位址）、`-u`（使用者空間）、`-a`（ASCII）、`-32/-64`、`-s`（顯示符號） |
| `wr` | 寫記憶體（**活系統才有意義，慎用**） | — |
| `struct` | 印資料結構定義或內容（§4.4-7） | `struct <型別>`（定義）、`struct <型別> -o`（**印每個成員的偏移量**）、`struct <型別> <addr>`（照型別解讀）、`struct <型別>.<成員> <addr> -x`（只印某成員、十六進位） |
| `union` / `whatis` | 同上（union）／查型別 | `whatis <型別或符號>` |
| `p` | 印變數或運算式（等於 gdb 的 print） | `p jiffies`、`p init_mm`、`p *(struct task_struct *)0xffff…` |
| `list` | **走訪鏈結串列** | `list -H <head>`（從 list_head 開始）、`list -s <型別>.<成員> -H <addr>`（每個節點順便印成員）、`list -o <offset>` |
| `tree` | 走訪紅黑樹／radix tree | `tree -t rbtree <根節點位址>`；完整語法看 `help tree` |
| `kmem` | 記憶體統計（§4.4-12） | `kmem -i`（總覽）、`kmem -s`（slab）、`kmem -p`（page）、`kmem <addr>`（這個位址屬於誰） |
| `vm` | 行程位址空間 | `vm <pid>`、`vm -p`（含 VA→PA） |
| `vtop` / `ptov` | 虛擬↔實體位址轉換 | `vtop <vaddr>` |
| `search` | 在記憶體裡搜尋某個值 | `search -k <value>`（核心空間） |

### 8.4 程式碼與符號

| 命令 | 用途 | 常用選項 |
|---|---|---|
| `dis` | 反組譯（§4.4-3） | `dis <func>`、`dis -l <addr>`（**附行號**）、`dis -s`（附原始碼） |
| `sym` | 符號查詢（§4.4-5） | `sym <addr>`（位址→名稱）、`sym -q <名稱>`（模糊查詢）、`sym -m <module>`（模組全部符號）、`sym -l`（等於 System.map） |
| `mod` | 模組管理（§4.4-4） | `mod`（列出）、`mod -s <name> <path.ko>`（**載入模組符號，分析模組崩潰必做**）、`mod -S <dir>`（整個目錄） |
| `eval` / `pd` | 算式求值 | `eval 0xffff-0x10`、`pd (a - b)`（十進位印出） |

### 8.5 中斷、鎖、其他

| 命令 | 用途 |
|---|---|
| `irq` | 中斷描述子、`irq -s` 統計、`irq -d` 顯示 irq_desc |
| `timer` | 計時器列表 |
| `dev` | 裝置 / I/O 資源 |
| `net` | 網路裝置與 socket |
| `ipcs` | System V IPC |
| `swap` | 交換分割區 |
| `extend` | 載入外掛（如 `eppic`） |
| `q` / `exit` | 離開 |

> 💡 **`help <命令>` 永遠是最快的參考**：`help bt`、`help list`、`help struct` 都有完整選項與範例。

---

<a name="s9"></a>
## 9. 實戰流程 1：拿到 vmcore 的前五分鐘

```
crash> log | tail -50          # 1. 崩潰前的 dmesg（也可以直接看 vmcore-dmesg.txt）
crash> sys                     # 2. 確認版本、記憶體、PANIC 原因
crash> bt                      # 3. 崩潰當下那個行程的堆疊
crash> bt -a                   # 4. 每顆 CPU 當時在做什麼（找「另一個兇手」）
crash> ps | grep -c UN         # 5. 有幾個 D 狀態行程？很多的話往死鎖／IO 方向查
crash> ps | grep RU            # 6. 誰在跑
crash> kmem -i                 # 7. 記憶體還夠嗎（OOM 類問題）
crash> mod                     # 8. 載了哪些外掛模組（第三方驅動最可疑）
```

**判斷分支**（看 `PANIC:` 那一行）：

| PANIC 內容 | 往哪走 |
|---|---|
| `NULL pointer dereference` / `Unable to handle kernel paging request` / `Oops` | → [流程 2](#s10) |
| `softlockup: hung tasks` | 有 CPU 被長時間占住 → `bt` 直接看那顆 CPU 在跑什麼迴圈 |
| `hung_task: blocked tasks` | 有行程 D 狀態太久 → [流程 3](#s11) |
| `Out of memory` / `oom-killer` | → [流程 7](#s15) |
| `SysRq` | 人為觸發（測試用） |

---

<a name="s10"></a>
## 10. 實戰流程 2：空指標／oops 類崩潰

📕 書上 §5.2（ARM64 版，行 88-131）與 §4.5（x86_64 版）的完整流程：

```
crash> bt
  …
  #9 [ffff910701907da0] page_fault at ffffffff8ad6b758
  [exception RIP: sysrq_handle_crash+22]          ← ★ 出事的指令位址
  RIP: ffffffff8aa61e66  RSP: ffff910701907e58  RFLAGS: 00010246
  RAX: … RBX: … RDI: 0000000000000063             ← ★ 出事當下的暫存器
```

1. **找出錯指令**：`bt` 裡的 `exception RIP`（x86）或 `pc :`（ARM64）。
2. **載入模組符號**（崩在模組裡才需要）：
   ```
   crash> mod -s oops /home/user/oops.ko
   ```
3. **反組譯出錯的地方**：
   ```
   crash> dis -l ffff000000e54020
   0xffff000000e54020 <create_oops+32>:   ldr  x0, [x0,#80]
   ```
4. **把偏移量翻譯成成員**：
   ```
   crash> struct -o vm_area_struct
   struct vm_area_struct {
       [0]  unsigned long vm_start;
       …
       [80] unsigned long vm_flags;      ← 0x50 = 80 → 這行在存取 vma->vm_flags
   }
   ```
5. **看那個指標是什麼**：ARM64 第 1 個參數在 `x0`，x86_64 在 `RDI`。
   ```
   crash> struct vm_area_struct 0x0
   struct: invalid kernel virtual address: 0x0       ← 果然是空指標
   ```
6. **定位到原始碼行**：`dis -l` 有行號；或在殼層用
   `./scripts/faddr2line oops.ko create_oops+0x20`。

🔧 本機沒有 crash 可用時的等價做法（📝 [ch14 Q3](./ch14_arm64_crash_debugging.md#q3)）：

```bash
# oops 訊息本身就有 pc/lr + Call trace
sudo dmesg | grep -A20 "Unable to handle"
# 模組載入基底 + .ko 內偏移
cat /sys/module/oops_lab/sections/.text        # 0xffff80000125c000
nm oops_lab.ko | grep " t create_oops"
# 位址 → 原始碼行
~/disk/kernel-source/scripts/faddr2line oops_lab.ko create_oops+0x4c
#   → create_oops at oops_lab.c:46
```

---

<a name="s11"></a>
## 11. 實戰流程 3：系統卡死、一堆 D 狀態行程

📕 書上 §4.11（行 2891-2960）與 §5.5：

```
crash> ps | grep UN                       # 1. 找出所有 D 狀態行程
5518   2   2  ffff9494060ab0c0  UN  0.0  …  [lock_test1]
5522 2165  2  ffff94940f64b0c0  UN  0.0  …  test
5523 2165  1  ffff94940f64d140  UN  0.1  …  ps

crash> foreach UN bt > /tmp/un.log        # 2. 一次把它們的堆疊全部倒出來
crash> ! grep -c __schedule /tmp/un.log   # 3. 統計卡在哪（! 表示執行殼層命令）

crash> bt 5523                            # 4. 逐個看，找共同的等待點
  #3 rwsem_down_read_failed …             ← 卡在讀寫信號量
  #5 proc_pid_cmdline_read …              ← 是在讀 /proc/<pid>/cmdline
```

**判讀心法**：

- 堆疊裡出現 `rwsem_down_read_slowpath` / `rwsem_down_write_slowpath` / `__mutex_lock` → **鎖**問題 → [流程 5](#s13)
- 出現 `io_schedule` / `wait_on_page_bit` / `submit_bio` → **I/O** 卡住（硬碟、網路檔案系統）
- 出現 `flush_work` / `__cancel_work_timer` → **workqueue 死鎖**
- 只有 `khungtaskd` 自己 panic，其它行程都正常 → 看 `log` 裡 hung_task 印的那條堆疊

🔧 本機等價做法：`echo ps > /proc/crash_probe`（[`crash_probe.ko`](./experiments/crash_probe.c)），
它照 `kernel/hung_task.c:203-210` 的判斷式過濾掉 `TASK_IDLE`／`TASK_KILLABLE`，
不然會被上百個閒置 kworker 洗版。

---

<a name="s12"></a>
## 12. 實戰流程 4：從堆疊推導區域變數與參數

這是 crash 用得最深的一招（書上 §4.10、§5.4、§5.5.2）。**核心觀念**：
變數在堆疊上的位置是「框架指標 ± 偏移量」，偏移量從**反組譯**讀出來，框架指標從 **`bt`** 讀出來。

### x86_64 版（書上 §4.10，行 2380-2460）

```
crash> bt -f 4304                        # 1. 找到目標函式框架的「返回位址欄位」
  #6 [ffff8c4bc4aefc98] init_module …    #    → 0xffff8c4bc4aefd38
crash> mod -s oops /home/user/oops.ko    # 2. 載模組符號
crash> dis init_module                   # 3. 反組譯，找 lea
  <init_module+233>:  lea  -0x68(%rbp),%rcx     ← priv 在 rbp-0x68
  <init_module+244>:  mov  %rcx,%rsi            ← 當第 2 個參數
# 4. 套公式（RBP = 返回位址欄位 − 8）
#    priv = 0xffff8c4bc4aefd38 − 0x8 − 0x68 = 0xffff8c4bc4aefcc8
crash> rd ffff8c4bc4aefcc8
ffff8c4bc4aefcc8:  000000006f676966    figo....
crash> struct mydev_priv ffff8c4bc4aefcc8
struct mydev_priv { name = "benshushu\000", i = 10, mm = 0x…, sem = 0x… }
```

**心法**：看到 `lea`（取址）→ 那格**就是**變數本體；看到 `mov`（讀值）→ 那格存的是**指標**，要多 `rd` 一次。

### ARM64 版（書上 §5.4、§5.5.2）

ARM64 前 8 個參數走 `x0`~`x7`，而且常被搬進 **callee-saved 的 `x19`~`x28`**，
**堆疊上往往一個參數都沒有**。這時要往**子函式**的框架裡找——因為子函式用到 x19~x28 前一定得先存起來。

🔧 本機實測（📝 [ch14 Q4](./ch14_arm64_crash_debugging.md#q4)，4 個參數 4/4 命中）：

```bash
# 1. bt 得到 rwsem_down_read_slowpath 的框架記錄 = 0xffff80001056bd10
# 2. 反組譯它的序幕（crash 用 dis，這裡用 objdump + 重建的 vmlinux）
$ objdump -d vmlinux | grep -A5 "<rwsem_down_read_slowpath>:"
  sub  sp, sp, #0x90
  stp  x29, x30, [sp, #80]
  add  x29, sp, #0x50          ← x29 = sp + 0x50 ⇒ sp = x29 − 0x50 = 0xffff80001056bcc0
  stp  x19, x20, [sp, #96]     ← 呼叫者的 x19、x20 存在 sp+96、sp+104
  stp  x21, x22, [sp, #112]    ← x21、x22 存在 sp+112、sp+120
# 3. 讀出來
crash> rd ffff80001056bd20 4
ffff80001056bd20:  ffff800001101030    ← x19 = sem
ffff80001056bd28:  ffff80001056bdf0    ← x20 = priv
ffff80001056bd30:  1122334455667788    ← x21 = magic
ffff80001056bd38:  0000000000008888    ← x22 = 第 8 個參數
```

**ARM64 心法**（三條公式）：

```
sp        = x29 − M              （M 來自序幕的 add x29, sp, #M）
局部變數   = x29 ± K              （K 來自 sub xN,x29,#K 或 str …,[x29,#-K]）
參數       = 去子函式的 stp x19,x20,[sp,#N] 找備份
```

---

<a name="s13"></a>
## 13. 實戰流程 5：誰持有鎖、誰在等鎖

### 5-1 找到鎖的位址

從 `bt` 看到卡在 `down_read`/`down_write`/`mutex_lock`，用[流程 4](#s12) 的方法把**第 1 個參數**（鎖的位址）挖出來。

### 5-2 看鎖的狀態（誰持有）

```
crash> struct rw_semaphore.count,owner,wait_list ffff800001101030 -x
```

**判讀**（⚠ 版本差異很大，見 📝 [ch14 Q5](./ch14_arm64_crash_debugging.md#q5)）：

| 核心版本 | `count` | `owner` |
|---|---|---|
| 3.10 / 5.0（書上） | `0xffffffff00000001` 這種「高位是等待者、低位是持有者」 | 指標 = 寫者持有；**`1` = 讀者持有** |
| **5.3 以後（含本機 6.1）** | **位元編碼**：bit0 = 寫者持有、bit1 = 有等待者、bit2 = handoff、bit63 = readfail、**讀者數在 bit8 以上**。🔧 本機「一個寫者持有 + 有人等」= **`0x3`** | `atomic_long_t`，**低 3 位是旗標**（bit0 = 讀者持有、bit1 = 不可自旋）；高位才是 `task_struct` 指標 |

```
crash> struct task_struct.comm,pid <owner 的高位部分>
```

> 讀者鎖的 `owner` **只記錄最後一個讀者**，不是完整清單——這是 rwsem 比 mutex 難查的根本原因。

### 5-3 看誰在等（走訪 wait_list）

```
crash> list -s rwsem_waiter.task,type -H <sem 位址 + wait_list 偏移>
ffff00000b26bda8   task = 0xffff8000613e3900   type = RWSEM_WAITING_FOR_WRITE
ffff00000b273bb8   task = 0xffff8000613e0000   type = RWSEM_WAITING_FOR_READ
crash> struct task_struct.comm,pid 0xffff8000613e3900
```

🔧 本機實測小技巧：`rwsem_waiter` 是 `rwsem_down_*_slowpath()` 的**區域變數**，
所以它的位址一定落在**等待者自己的核心堆疊**裡——反過來可以用位址判斷是誰在等。

### 5-4 若鎖是內嵌在別的結構裡（例如 `mm->mmap_lock`）

```
crash> struct -o mm_struct | grep mmap          # 查偏移量（3.10=0x78、5.0=0x60、本機 6.1=0x88）
crash> struct mm_struct.owner <鎖位址 − 偏移量>  # 反推 mm_struct，再找它的擁有者
```

---

<a name="s14"></a>
## 14. 實戰流程 6：一個行程被阻塞了多久

書上 §4.11.4（行 3242-3304）：

```
crash> set 5518                      # 切到目標行程
crash> task -R sched_info
sched_info = {
  pcount = 8,
  run_delay = 3921156731,
  last_arrival = 1658412338927,      ← 最後一次真正跑在 CPU 上的時間戳（ns）
  last_queued = 0
}
crash> runq -t                       # 各 CPU 的當前時間戳
CPU 2: 1755597424093 …
crash> pd (1755597424093 - 1658412338927)
$2 = 97185085166                     ← 97.19 秒
```

⚠ 兩個常見陷阱：

1. **`last_arrival` 是 0**：5.x 之後 sched_info 被 static key 管控，
   要先 `sysctl -w kernel.sched_schedstats=1`（🔧 本機預設是 0，重開機會恢復）。
2. **時間基準要同源**：`last_arrival` 用的是 `sched_clock`；
   🔧 本機實測 `sched_clock` 與 `/proc/uptime` 一致，但 **printk 時間戳快了約 13 秒**，
   所以**不能**拿 dmesg 的時間去減。

🔧 本機驗證結果（📝 [ch14 Q7](./ch14_arm64_crash_debugging.md#q7)）：算出 **3276.599 秒**，實際經過 3276.9 秒 ✓

---

<a name="s15"></a>
## 15. 實戰流程 7：記憶體吃光了

```
crash> kmem -i                    # 總覽：free / used / slab / cached / swap
crash> kmem -s | head -30         # 各 slab cache 的用量（找異常膨脹的）
crash> kmem -s <cache名>          # 單一 cache 細節
crash> kmem <addr>                # 這個位址屬於哪個 page / slab / vmalloc 區
```

書上 §4.12（行 3307-3330）的兩個統計技巧：

```
crash> ps -u | awk '{ total += $8 } END { printf "Total RSS: %.02f GB\n", total/2^20 }'
Total RSS of user-mode: 0.74 GB

crash> ps -u | awk '{ m[$9]+=$8 } END { for (i in m) printf "%20s %10s KB\n", i, m[i] }' | sort -k2 -rn | head
               crash     296848 KB
               gmain      79936 KB
```

其他方向：

| 症狀 | 命令 |
|---|---|
| slab 洩漏 | `kmem -s`，找 `ALLOCATED` 遠大於預期的 cache；再 `kmem -S <cache>` 看物件 |
| vmalloc 洩漏 | `kmem -v` |
| 大量 page cache | `kmem -i` 的 `CACHED`，配合 `files` |
| 某個行程吃太多 | `ps -u` 排序、`vm <pid>` 看 VMA |

---

<a name="s16"></a>
## 16. 實戰流程 8：批次化與腳本化

crash 支援把命令寫成檔案批次執行，做自動化分析報告很好用：

```bash
cat > /tmp/cmds.txt <<'EOF'
sys
log
bt
bt -a
ps | grep UN
foreach UN bt
kmem -i
mod
quit
EOF

crash -s -i /tmp/cmds.txt vmlinux vmcore > /tmp/report.txt 2>&1
```

| 選項 | 用途 |
|---|---|
| `-i <file>` | 從檔案讀命令 |
| `-s` | 安靜模式（不印開場橫幅、不分頁），**腳本必用** |
| `-e vi\|emacs` | 命令列編輯模式 |
| `--machdep <opt>=<val>` | 覆寫架構參數（ARM64：`phys_offset`、`kimage_voffset`、`vabits_actual`、`max_physmem_bits`） |
| `-d <n>` | 除錯輸出等級（**排錯時很有用**，見[第 19 節](#s19)） |

crash 內部也能：

```
crash> ! ls -l /tmp            # ! 開頭 = 執行殼層命令
crash> bt > /tmp/bt.log        # 輸出重導
crash> bt | grep rwsem         # 管線
crash> alias uns "ps | grep UN"   # 自訂別名
crash> repeat -1 ps            # 重複執行（活系統監看用）
```

---

<a name="s17"></a>
## 17. ARM64 專屬注意事項

| 項目 | x86_64 | ARM64 | 影響 |
|---|---|---|---|
| 框架指標 | `RBP`，`[RBP]`=父、`[RBP+8]`=返回位址 | `x29`，`[x29]`=父、`[x29+8]`=LR（**同構**） | `bt` 的原理一樣 |
| 框架指標位置 | 固定在框架頂端，變數在 `RBP−x` | **不固定**：可能 `x29==sp`，也可能 `add x29,sp,#M`；變數在 `x29±x` | 推導變數時**一定要看序幕** |
| 葉子函式 | 至少返回位址在堆疊 | **可能完全不建框架記錄** | 🔧 **calltrace 會少一層**，名字只在 `lr :` 那行（📝 [ch14 Q3](./ch14_arm64_crash_debugging.md#q3)） |
| 參數 | 前 6 個走暫存器，常 spill 回堆疊 | 前 8 個走 `x0~x7`，常搬到 `x19~x28` | **參數要去子函式的框架裡撈**（[流程 4](#s12)） |
| 返回位址 | `call` 自動 push | `bl` 只寫 LR | 出錯函式的父函式可能只存在 LR 裡 |
| 位址→符號 | — | 核心用 `%pSb` → `sprint_backtrace()` **減 1**；書上教的是**減 4**（一條 A64 指令） | 想找「呼叫點那條指令」要自己減 4 |
| VA 佈局 | — | 5.11 之後**翻轉**：核心映像 `0xffff8000…`、線性映射 `0xffff0000…` | ⚠ 沒有 VMCOREINFO 時 crash 會猜錯 VA_BITS（見[第 19 節](#s19)） |
| 暫存器現場 | `bt` 顯示 `exception RIP` + 通用暫存器 | `bt` 顯示 `pc`/`lr`/`sp` + `x0~x30`；使用者態被打斷時還會有 `ORIG_X0`/`SYSCALLNO`/`PSTATE` | — |

---

<a name="s18"></a>
## 18. 本機（RK3588）現況與替代方案

🔧 這塊板子的 Rockchip 6.1.115 核心：

```
# CONFIG_KEXEC is not set          ← 沒有 kexec，永遠不會有 vmcore
# CONFIG_CRASH_DUMP is not set
# CONFIG_PROC_KCORE is not set     ← crash 也沒辦法分析活系統
CONFIG_STRICT_DEVMEM=y             ← /dev/mem 被限制
CONFIG_DEBUG_INFO_NONE=y           ← 原廠沒有 vmlinux（我們自己重建了一個）
```

所以本專案寫了 [`crash_probe.ko`](./experiments/crash_probe.c) 當替身，對照表：

| crash 子命令 | crash_probe 指令 | 說明 |
|---|---|---|
| `ps \| grep UN` | `echo ps > /proc/crash_probe` | 已過濾 `TASK_IDLE`/`TASK_KILLABLE` |
| `bt <pid>` | `bt <pid>` | 走 x29 框架鏈 |
| `bt -f <pid>` | `btf <pid>` | 逐格 dump + 符號標註 |
| `rd <addr>` | `rd <hex> [n]` | 讀核心記憶體 |
| `struct rw_semaphore` + `list -s rwsem_waiter` | `rwsem <hex>` | 一次印出 count 解碼、owner、全部等待者 |
| `struct mm_struct.owner` | `mm <pid>` / `mmowner <hex>` | 含偏移量反推 |
| `task -R sched_info` + `runq -t` | `sched <pid>` / `runq` | 直接算出阻塞時間 |
| `dis` | `objdump -d vmlinux`／`objdump -d xxx.ko` | 用重建的 vmlinux |
| `sym` | `/proc/kallsyms` + `/sys/module/*/sections/.text` + `nm` | — |
| `log` | `dmesg` | — |

> **`crash_probe` 反而有一個 crash 沒有的優點**：它不碰 `/proc/<pid>/`，
> 所以當 `mmap_lock` 被卡死、`ps`/`top`/`pgrep` 全部掛住時，它照樣能用。

**要讓真的 crash 在這塊板子上跑起來**，最短路徑是重編核心打開
`CONFIG_KEXEC=y`、`CONFIG_CRASH_DUMP=y`、`CONFIG_PROC_KCORE=y`、`CONFIG_DEBUG_INFO_DWARF4=y`，
再加 `crashkernel=256M` 開機參數。

---

<a name="s19"></a>
## 19. 錯誤訊息排錯表

| 錯誤訊息 | 原因 | 解法 |
|---|---|---|
| `crash: cannot find a live memory device` | 沒有 `/dev/crash`、`/dev/mem` 被 `CONFIG_STRICT_DEVMEM` 擋、也沒有 `/proc/kcore` | 編 crash 的 memory driver（[第 6 節](#s6)），或打開 `CONFIG_PROC_KCORE` |
| `crash: vmlinux and vmcore do not match!` | ① 版本橫幅不同 ② vmlinux 不是同一次編譯的 ③ 位址轉換錯誤導致讀到垃圾 | `strings vmlinux \| grep -m1 "^Linux version"` 與 `/proc/version`（或 `vmcore-dmesg.txt` 第一行）逐字比對 |
| `crash: cannot determine VA_BITS_ACTUAL: please use /proc/kcore` | ARM64 活系統分析，核心有 `vabits_actual` 符號但讀不到 | 打開 `CONFIG_PROC_KCORE=y`，或改走 vmcore 路線 |
| ⚠ `WARNING: could not find MAGIC_START!` 之後 `do not match` | 🔧 本機遇到的：crash 猜錯 VA_BITS（47 vs 48），5.11+ 的 ARM64 翻轉佈局 + 沒有 VMCOREINFO | `--machdep vabits_actual=48 --machdep phys_offset=0x200000` 有機會救；本機救不了 → 正解是把 Kdump 補起來 |
| `struct: invalid data structure reference: mydev_priv` | 那個結構在模組裡，符號沒載入 | `mod -s <模組名> <路徑/xxx.ko>` |
| `crash: <addr>: kernel virtual address not found` | ① 位址算錯 ② `[PARTIAL DUMP]` 把該頁濾掉了 ③ 該頁未映射 | 用 `vtop <addr>` 檢查；makedumpfile 的 `-d` 等級調低一點重抓 |
| `bt: WARNING: possibly bogus exception frame` | 堆疊被破壞，或框架指標被最佳化掉 | 改用 `bt -t`（掃描所有 text 符號）交叉比對 |
| `crash: page excluded: kernel virtual address` | makedumpfile 過濾掉了（`-d 31` 會濾很多） | 重抓時用 `-d 1` 或 `-d 0` |
| 開場 `PANIC:` 是 `khungtaskd` 之類的內部執行緒 | 那只是「發現者」不是「兇手」 | `ps \| grep UN` + `foreach UN bt` 找真正卡住的行程 |
| 命令打完沒反應 / 一直分頁 | crash 預設用 `less` 分頁 | `crash -s`，或在命令後加 `| cat`，或設 `set scroll off` |

---

<a name="s20"></a>
## 20. 附錄：延伸閱讀

**書上對應章節**（`books/running-linux-kernel/running-kernel-2-txt/`）：

| 主題 | 檔案 : 行號 |
|---|---|
| Kdump 工作原理 | `11_第4章…txt:32-60` |
| CentOS 安裝與設定 Kdump | `11_第4章…txt:211-309` |
| **crash 命令逐一介紹**（約 50 個） | `11_第4章…txt:310-1004` |
| 案例：簡單宕機 / 存取已刪除鏈結串列 / 真實驅動崩潰 | `11_第4章…txt:1005-1926` |
| 死鎖檢查機制（softlockup/hardlockup/hung_task） | `11_第4章…txt:1927-2004` |
| **推導參數與區域變數**（x86_64） | `11_第4章…txt:2121-2478` |
| **複雜宕機案例：鎖的持有者與等待者** | `11_第4章…txt:2479-3304` |
| crash 調試技巧彙總 | `11_第4章…txt:3305-` |
| **ARM64 版：恢復函式呼叫棧、推導參數** | `12_第5章…txt:132-370` |
| ARM64 版：複雜宕機案例 | `12_第5章…txt:371-741` |

**本專案的實測筆記**：

- 📝 [ch13 基於 x86_64 解決宕機難題](./ch13_x86_64_crash_debugging.md)（Kdump 原理、三個 lockup 偵測器、x86_64 推導）
- 📝 [ch14 基於 ARM64 解決宕機難題](./ch14_arm64_crash_debugging.md)（ARM64 框架鏈、calltrace、參數推導、**附錄 A 有把 crash 搬上板子的完整紀錄**）
- 📝 [ch12 內核調試與性能優化](./ch12_kernel_debugging_and_performance_optimization.md)（oops 解讀、`decodecode`、`faddr2line`、模組重定位）

**實驗程式**：

- [`experiments/build_vmlinux.sh`](./experiments/build_vmlinux.sh) — 重建與執行中核心相符的 vmlinux
- [`experiments/crash_probe.c`](./experiments/crash_probe.c) — crash 子命令的核心模組替身
- [`experiments/arm64_lab.c`](./experiments/arm64_lab.c) — 造一個「答案已知」的死鎖現場，用來練習上面所有流程

**官方資源**：

- `man crash`、crash 內的 `help <命令>`
- 原始碼與 memory driver：<https://github.com/crash-utility/crash>
- 白皮書：crash 原始碼樹裡的 `crash_whitepaper`
