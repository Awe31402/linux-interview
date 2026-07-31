#!/bin/sh
# Ch4 Q13/Q16/Q17：從 sysfs 讀 SLUB 每個 kmem_cache 的內部參數
# 用法： sudo ./slub_info.sh
for d in kmalloc-128 kmalloc-1k dentry task_struct vm_area_struct anon_vma; do
  [ -d /sys/kernel/slab/$d ] || continue
  printf "%-16s object_size=%-6s slab_size=%-6s order=%s objs_per_slab=%-4s cpu_partial=%-4s align=%-4s reclaim=%s\n" \
    "$d" "$(cat /sys/kernel/slab/$d/object_size)" "$(cat /sys/kernel/slab/$d/slab_size)" \
    "$(cat /sys/kernel/slab/$d/order)" "$(cat /sys/kernel/slab/$d/objs_per_slab)" \
    "$(cat /sys/kernel/slab/$d/cpu_partial)" "$(cat /sys/kernel/slab/$d/align)" \
    "$(cat /sys/kernel/slab/$d/reclaim_account)"
done
echo "--- kmalloc 的尺寸階梯 ---"
ls /sys/kernel/slab/ | grep -E '^kmalloc-[0-9]' | sort -t- -k2 -n | tr '\n' ' '; echo
echo "--- kmalloc-128 被哪些 cache 合併進來 [SLAB_MERGE_DEFAULT] ---"
cat /sys/kernel/slab/kmalloc-128/aliases
echo "--- ARCH_KMALLOC_MINALIGN 的效果：最小的 kmalloc cache ---"
ls /sys/kernel/slab/ | grep -E '^kmalloc-[0-9]+$' | sed 's/kmalloc-//' | sort -n | head -1
