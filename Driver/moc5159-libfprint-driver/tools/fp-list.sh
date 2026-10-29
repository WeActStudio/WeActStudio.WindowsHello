#!/usr/bin/env bash
#
# 查看当前指纹状态：fprintd 里登记了哪些 + 芯片库里实际存了几条记录
#
#   ./tools/fp-list.sh
#   sudo ./tools/fp-list.sh        # 额外读取芯片库（推荐）
#
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
PKG_ROOT=$(cd "$HERE/.." && pwd)
U="${FPRINT_USER:-${SUDO_USER:-$(id -un)}}"

echo "== fprintd 里用户 $U 的指纹 =="
if command -v fprintd-list >/dev/null 2>&1; then
    fprintd-list "$U" 2>&1 | tail -12
else
    echo "  没有 fprintd-list（apt install fprintd）"
fi

echo
echo "== 存储文件（/var/lib/fprint） =="
if [ "$(id -u)" -eq 0 ]; then
    ls -la "/var/lib/fprint/$U/moc5159/"*/ 2>/dev/null | tail -12 || echo "  （空）"
else
    echo "  需要 root 才能读"
fi

echo
echo "== 芯片模板库 =="
PROBE=""
for p in "$PKG_ROOT/build/probe" "$(command -v moc5159-probe 2>/dev/null)"; do
    [ -x "$p" ] && { PROBE="$p"; break; }
done
if [ -z "$PROBE" ]; then
    echo "  没有 probe（sudo $PKG_ROOT/scripts/build-probe.sh 编译）"
elif [ "$(id -u)" -ne 0 ]; then
    echo "  需要 root：sudo $0"
else
    systemctl stop fprintd >/dev/null 2>&1
    "$PROBE" list 2>&1 | tail -14
    systemctl start fprintd >/dev/null 2>&1
    echo
    echo "  提示：本固件只比对第一条记录，多条模板时只有 index[0] 生效。"
fi
