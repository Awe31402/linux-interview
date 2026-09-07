#!/bin/bash
# exp03_count.sh — 實驗 3.1
#
# 問題：一次推論到底送了哪些 ioctl、各幾次？
#
# 關鍵技巧：strace 會把 DRM 驅動私有的 ioctl 叫錯名字（編號在各驅動間重複）。
#           加上 -e raw=ioctl 讓它印「原始號碼」，就不會被名字騙。
#
#   ./exp03_count.sh <模型> <圖片>

set -e
DEMO=${DEMO:-./rknn_create_mem_demo}
MODEL=${1:-model/RK3588/mobilenet_v1.rknn}
IMG=${2:-model/dog_224x224.jpg}
TR=/tmp/exp03_trace.txt

strace -f -e trace=ioctl -e raw=ioctl -o "$TR" "$DEMO" "$MODEL" "$IMG" >/dev/null 2>&1

echo "=== 一次推論送出的 ioctl ==="
grep -oE 'ioctl\(0x[0-9a-f]+, 0x[0-9a-f]+' "$TR" | awk '{print $2}' | sort | uniq -c | sort -rn |
while read -r n cmd; do
    c=$((cmd))
    type=$(( (c >> 8) & 0xff ))
    nr=$((   c        & 0xff ))
    size=$(( (c >> 16) & 0x3fff ))
    name="(不是 rknpu 的)"
    if [ "$type" -eq 100 ] && [ "$nr" -ge 64 ]; then
        case $nr in
            64) name="RKNPU_ACTION" ;;
            65) name="RKNPU_SUBMIT" ;;
            66) name="RKNPU_MEM_CREATE" ;;
            67) name="RKNPU_MEM_MAP" ;;
            68) name="RKNPU_MEM_DESTROY" ;;
            69) name="RKNPU_MEM_SYNC" ;;
            *)  name="(rknpu 未定義的 nr=$nr)" ;;
        esac
    elif [ "$type" -eq 100 ]; then
        # include/uapi/drm/drm.h
        case $nr in
            0)  name="DRM_IOCTL_VERSION（核心）" ;;
            1)  name="DRM_IOCTL_GET_UNIQUE（核心）" ;;
            9)  name="DRM_IOCTL_GEM_CLOSE（核心）" ;;
            10) name="DRM_IOCTL_GEM_FLINK（核心）" ;;
            11) name="DRM_IOCTL_GEM_OPEN（核心）" ;;
            45) name="DRM_IOCTL_PRIME_HANDLE_TO_FD（核心）" ;;
            46) name="DRM_IOCTL_PRIME_FD_TO_HANDLE（核心）" ;;
            *)  name="(DRM 核心 ioctl, nr=$nr)" ;;
        esac
    fi
    printf "%5d 次  cmd=%-12s nr=0x%02x size=%-4d  %s\n" "$n" "$cmd" "$nr" "$size" "$name"
done
