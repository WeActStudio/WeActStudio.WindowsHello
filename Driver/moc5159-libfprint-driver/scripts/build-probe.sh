#!/usr/bin/env bash
#
# 编译独立诊断工具 probe（只依赖 libusb，不依赖 libfprint / fprintd）
#
#   ./scripts/build-probe.sh            # 编译到 <包>/build/probe
#   sudo ./scripts/build-probe.sh -i    # 顺便装到 /usr/local/bin/moc5159-probe
#
set -euo pipefail

PKG_ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUTDIR="$PKG_ROOT/build"
INSTALL=0
[ "${1:-}" = "-i" ] && INSTALL=1

command -v cc >/dev/null 2>&1 || { echo "缺少 cc（apt install build-essential）" >&2; exit 1; }
pkg-config --exists libusb-1.0 2>/dev/null || {
    echo "缺少 libusb 开发包：" >&2
    echo "  Debian/Ubuntu : sudo apt install libusb-1.0-0-dev" >&2
    echo "  Fedora        : sudo dnf install libusb1-devel" >&2
    echo "  Arch          : sudo pacman -S libusb" >&2
    exit 1
}

mkdir -p "$OUTDIR"
cc -O2 -Wall -Wextra -I "$PKG_ROOT/src" \
   -o "$OUTDIR/probe" \
   "$PKG_ROOT/tools/moc5159-probe.c" "$PKG_ROOT/src/moc5159-proto.c" \
   $(pkg-config --cflags --libs libusb-1.0)

echo "已编译: $OUTDIR/probe"
echo
echo "常用命令（需要 root，且先停掉 fprintd）:"
echo "  sudo systemctl stop fprintd"
echo "  sudo $OUTDIR/probe info      # 固件版本 / 传感器信息"
echo "  sudo $OUTDIR/probe db        # 库头状态（ev=0x27 正常）"
echo "  sudo $OUTDIR/probe list      # 枚举设备内模板"
echo "  sudo $OUTDIR/probe ident -t30# 走一遍比对流程（会等手指）"
echo "  sudo systemctl start fprintd"

if [ "$INSTALL" -eq 1 ]; then
    install -m 0755 "$OUTDIR/probe" /usr/local/bin/moc5159-probe
    echo
    echo "已安装: /usr/local/bin/moc5159-probe"
fi
