#!/usr/bin/env bash
# Flash the uKnomi capture-node firmware to a XIAO ESP32-S3 over USB.
#
#   ./flash.sh /dev/cu.usbmodemXXXX     # macOS
#   ./flash.sh /dev/ttyACM0             # Linux
#
# One firmware binary serves the whole fleet — per-unit identity and secrets live
# only on each device's microSD as /config.json (copy config.example.json).
set -euo pipefail

PORT="${1:-}"
if [[ -z "$PORT" ]]; then
    echo "usage: $0 <serial-port>" >&2
    echo "  find it with: ls /dev/cu.usbmodem* (macOS) or /dev/ttyACM* (Linux)" >&2
    exit 1
fi

if ! command -v idf.py >/dev/null 2>&1; then
    echo "idf.py not found — source the ESP-IDF env first: . \$IDF_PATH/export.sh" >&2
    exit 1
fi

idf.py set-target esp32s3
idf.py build
idf.py -p "$PORT" flash
echo
echo "Flashed. Now copy config.example.json to the SD card as /config.json,"
echo "fill in wifi + identity + broker, insert the card, and power-cycle."
echo "Watch logs with: idf.py -p $PORT monitor"
