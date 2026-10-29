#!/usr/bin/env bash
#
# build.sh — build the MOC-5159 module simulator
#
#   ./build.sh              # libmocsim.so + mocsim-selftest (+ mocsim-e2e if libfprint is present)
#   ./build.sh --no-e2e     # skip the libfprint end-to-end tester
#   ./build.sh --clean      # remove build output
#
# Requires a C compiler and libusb-1.0 development headers; the end-to-end
# tester additionally needs libfprint-2 (this driver's 1.94.100).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/build"
CC="${CC:-cc}"

E2E=1
for arg in "$@"; do
    case "$arg" in
        --clean)  rm -rf "$OUT"; echo "已清理 $OUT"; exit 0 ;;
        --no-e2e) E2E=0 ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "未知参数: $arg" >&2; exit 1 ;;
    esac
done

USB_CFLAGS="$(pkg-config --cflags libusb-1.0 2>/dev/null || echo -I/usr/include/libusb-1.0)"
USB_LIBS="$(pkg-config --libs libusb-1.0 2>/dev/null || echo -lusb-1.0)"

mkdir -p "$OUT"

echo "==> libmocsim.so"
# -pthread: the shim serialises its transfer queues with a mutex, and glibc
# before 2.34 does not have the pthread symbols in libc.
# shellcheck disable=SC2086
$CC -O2 -g -Wall -Wextra -fPIC -shared -pthread \
    -o "$OUT/libmocsim.so" \
    "$HERE/mocsim-shim.c" "$HERE/mocsim-dev.c" \
    $USB_CFLAGS

echo "==> mocsim-selftest"
# shellcheck disable=SC2086
$CC -O2 -g -Wall -Wextra \
    -o "$OUT/mocsim-selftest" \
    "$HERE/mocsim-selftest.c" \
    $USB_CFLAGS $USB_LIBS

if [ "$E2E" -eq 1 ]; then
    if pkg-config --exists libfprint-2 2>/dev/null; then
        echo "==> mocsim-e2e ($(pkg-config --modversion libfprint-2))"
        # shellcheck disable=SC2046
        $CC -O2 -g -Wall -Wextra \
            -o "$OUT/mocsim-e2e" \
            "$HERE/mocsim-e2e.c" \
            $(pkg-config --cflags --libs libfprint-2)
    else
        echo "==> 跳过 mocsim-e2e（没找到 libfprint-2，先装驱动再编）"
    fi
fi

echo
echo "构建完成："
ls -l "$OUT" | sed 's/^/  /'
echo
echo "自检（纯协议层，不插设备）："
echo "  LD_PRELOAD=$OUT/libmocsim.so $OUT/mocsim-selftest"
if [ -x "$OUT/mocsim-e2e" ]; then
    echo "端到端（libfprint + 驱动 + 模拟设备）："
    echo "  LD_PRELOAD=$OUT/libmocsim.so $OUT/mocsim-e2e"
fi
