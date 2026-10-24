#!/bin/bash
# 编译 / 烧录 / 看日志 一条龙
#   ./tools/build.sh            只编译
#   ./tools/build.sh flash      编译并烧录（自动找串口）
#   ./tools/build.sh monitor    只看串口日志（Ctrl+] 退出）
set -e
cd "$(dirname "$0")/../firmware"
. ~/esp/esp-idf-v5.5/export.sh >/dev/null
export IDF_TARGET=esp32s3
PORT=$(ls /dev/cu.usbserial-* /dev/cu.usbmodem* /dev/cu.SLAB_USBtoUART 2>/dev/null | head -1)
case "$1" in
  flash)   [ -n "$PORT" ] || { echo "没找到串口，板子插上了吗？用标有 UART 的 USB 口"; exit 1; }
           idf.py -p "$PORT" build flash monitor ;;
  monitor) [ -n "$PORT" ] || { echo "没找到串口"; exit 1; }
           idf.py -p "$PORT" monitor ;;
  *)       idf.py build ;;
esac
