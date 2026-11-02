#!/bin/bash
# agentboard 一键编译烧录：暂停推送服务 -> 编译 -> 烧录 -> 恢复服务
set -e
cd "$(dirname "$0")/firmware/agentboard"
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc"
PORT="${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"

echo "== compile =="
arduino-cli compile -b "$FQBN" . 2>&1 | grep -E 'error|Error|Sketch uses|Global variables' || true

if [ -z "$PORT" ]; then echo "!! 没找到 /dev/cu.usbmodem*，板子没连上"; exit 1; fi
echo "== upload to $PORT =="
launchctl bootout gui/$(id -u)/com.qiaodeli.agentboard 2>/dev/null || true
sleep 1
arduino-cli upload -b "$FQBN" -p "$PORT" . 2>&1 | grep -vE '^[\\/|-]+$|^Writing at|^Hash of|^\r' | tail -6
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.qiaodeli.agentboard.plist 2>/dev/null || true
echo "== done =="
