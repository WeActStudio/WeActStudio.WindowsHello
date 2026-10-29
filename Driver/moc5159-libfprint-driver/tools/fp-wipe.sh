#!/usr/bin/env bash
#
# ⚠️ 危险操作：清空芯片模板库 + 本机 fprintd 里这个用户的所有指纹
#
#   sudo ./tools/fp-wipe.sh
#
# 会做两件事：
#   1. 删掉 /var/lib/fprint/<用户>/moc5159/*/ 下的所有指纹文件
#   2. 给芯片发 0xD7（DB_WIPE）清空设备内模板库
#
# 什么时候需要它：
#   - 想换一根手指登记（本设备多指不生效，必须先清空再录新的）
#   - 库状态乱了（例如模板号/记录对不上，fprintd 一直说 no prints）
#
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
PKG_ROOT=$(cd "$HERE/.." && pwd)

[ "$(id -u)" -eq 0 ] || { echo "需要 root：sudo $0" >&2; exit 1; }

U="${FPRINT_USER:-${SUDO_USER:-}}"
[ -n "$U" ] && [ "$U" != "root" ] || { echo "无法确定用户名，请用 sudo 运行或指定 FPRINT_USER=<用户名>" >&2; exit 1; }

PROBE=""
for p in "$PKG_ROOT/build/probe" "$(command -v moc5159-probe 2>/dev/null)"; do
    [ -x "$p" ] && { PROBE="$p"; break; }
done
[ -n "$PROBE" ] || { echo "没有 probe，请先运行 sudo $PKG_ROOT/scripts/build-probe.sh" >&2; exit 1; }

echo "即将删除："
echo "  · /var/lib/fprint/$U/moc5159/*/ 下的全部指纹文件"
echo "  · 芯片内模板库（0xD7，不可恢复）"
echo
printf "确认请输入 yes: "
read -r ans
[ "$ans" = "yes" ] || { echo "已取消"; exit 0; }

systemctl stop fprintd >/dev/null 2>&1

echo
echo "== 清空前 =="
"$PROBE" list 2>&1 | tail -10

echo
echo "== 发 0xD7 清库 =="
"$PROBE" raw d7 2>&1 | tail -4

rm -f /var/lib/fprint/"$U"/moc5159/*/* 2>/dev/null
echo "已删除本机存储文件"

systemctl start fprintd >/dev/null 2>&1
sleep 1

echo
echo "== 清空后 =="
fprintd-list "$U" 2>&1 | tail -3
echo
echo "现在可以重新登记了：sudo $HERE/fp-enroll.sh right-index-finger 右手食指"
