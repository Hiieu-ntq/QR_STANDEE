#!/usr/bin/env bash
# Flash ESP32-S2 qua DFU (bootloader mode)
# Chạy khi mạch đang ở DFU bootloader (LED xanh nhấp nháy / USB PID 0x0002)

set -e

MERGED_BIN="$(dirname "$0")/build/merged_flash.bin"

echo "Kiểm tra thiết bị DFU..."
dfu-util --list 2>&1 | grep -i "303a:0002" || {
    echo "Không tìm thấy ESP32-S2 ở DFU mode!"
    echo "   → Giữ nút BOOT rồi nhấn RESET để vào DFU mode"
    exit 1
}

echo "Đang flash $MERGED_BIN ..."
dfu-util \
    --device 303a:0002 \
    --alt 0 \
    --dfuse-address 0x00000000 \
    --download "$MERGED_BIN" \
    --reset

echo ""
echo "Flash thành công! Mạch sẽ tự reset và chạy firmware."
