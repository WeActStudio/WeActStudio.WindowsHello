#!/usr/bin/env bash
#
# 登记一根指纹（带桌面按压提示，结束后自动关提示框）
#
#   sudo ./tools/fp-enroll.sh right-index-finger 右手食指
#   sudo ./tools/fp-enroll.sh left-thumb          左手拇指        # 中文名可省略
#
# 手指名可选值（libfprint 约定）：
#   left-thumb  left-index-finger  left-middle-finger  left-ring-finger  left-little-finger
#   right-thumb right-index-finger right-middle-finger right-ring-finger right-little-finger
#
# 注意：本设备固件只比对库内第一条记录，所以**只登记 1 根手指**最稳（见 docs/zh-CN/02-usage.md）。
#
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=/dev/null
. "$HERE/fp-prompt.sh"

F="${1:-}"
NAME="${2:-$F}"
[ -n "$F" ] || { echo "用法: sudo $0 <finger> [中文名]" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || { echo "需要 root：sudo $0 $*" >&2; exit 1; }

U="${FPRINT_USER:-${SUDO_USER:-}}"
if [ -z "$U" ] || [ "$U" = "root" ]; then
    echo "无法确定要登记到哪个用户名。请用 sudo 运行（保留 SUDO_USER），或指定 FPRINT_USER=<用户名>。" >&2
    exit 1
fi

command -v fprintd-enroll >/dev/null 2>&1 || {
    echo "没有 fprintd-enroll，请先安装 fprintd（apt install fprintd）" >&2; exit 1
}

trap prompt_stop EXIT INT TERM

prompt "登记准备" "即将登记【${NAME}】

按压要点：
· 指腹正中压在传感器上，不要用指尖或侧面
· 垂直下压，盖住整个传感器窗口
· 每次按实约 1 秒，然后完全抬离再按
· 一共需要 4~8 次

10 秒后自动开始" 12
sleep 10
prompt "登记中" "现在开始按压【${NAME}】

反复：按压 → 抬起 → 再按压，直到本窗口消失" 170

timeout 180 fprintd-enroll "$U" -f "$F" 2>&1
RC=$?
prompt_stop

echo "--------------------------------------------"
if [ "$RC" -eq 0 ]; then
    echo "登记结果: 成功"
else
    echo "登记结果: 失败 (exit=$RC) —— 常见原因是按压不一致或中途超时，重跑一次即可"
fi
echo "当前已登记:"
fprintd-list "$U" 2>&1 | tail -6

N=$(fprintd-list "$U" 2>/dev/null | grep -c '^ *[0-9]\+\.')
if [ "${N:-0}" -gt 1 ]; then
    echo
    echo "提示: 现在库里有 $N 根手指。本设备固件只比对库内第一条记录，"
    echo "      第二根及以后通常验证不过 —— 想换手指请先删掉旧的："
    echo "      sudo fprintd-delete $U -f <旧手指名>"
fi
