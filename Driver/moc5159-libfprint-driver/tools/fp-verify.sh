#!/usr/bin/env bash
#
# 验证一根指纹（带桌面按压提示，结束后自动关提示框）
#
#   sudo ./tools/fp-verify.sh right-index-finger 右手食指
#   ./tools/fp-verify.sh right-index-finger        # 普通用户也可以（走 polkit）
#
# 说明：不带 -f 的 `fprintd-verify` 只会比对列表第一条，容易给出假结论；
#       这里始终带 -f，并且额外打印驱动侧的“比对结果”。
#
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=/dev/null
. "$HERE/fp-prompt.sh"

F="${1:-}"
NAME="${2:-$F}"
[ -n "$F" ] || { echo "用法: sudo $0 <finger> [中文名]" >&2; exit 1; }

U="${FPRINT_USER:-${SUDO_USER:-$(id -un)}}"

trap prompt_stop EXIT INT TERM

prompt "验证准备" "3 秒后用【${NAME}】按压传感器" 6
sleep 3
prompt "请按压" "现在请按一下【${NAME}】，按实保持约 1 秒" 45

OUT=$(timeout 50 fprintd-verify "$U" -f "$F" 2>&1)
RC=$?
prompt_stop

echo "$OUT" | tail -3
echo "--------------------------------------------"
if echo "$OUT" | grep -qi 'verify-match'; then
    echo "结果: 匹配成功 (match)"
elif echo "$OUT" | grep -qi 'verify-no-match'; then
    echo "结果: 不匹配 (no-match) —— 手指没按正？再试一次；多次不过见 docs/zh-CN/03-troubleshooting.md"
elif echo "$OUT" | grep -qi 'verify-retry-scan'; then
    echo "结果: 采集质量不足，请重按"
else
    echo "结果: 未完成 (exit=$RC)"
fi
if [ "$(id -u)" -eq 0 ]; then
    echo
    echo "驱动侧记录:"
    journalctl -t fprintd --since '-2 min' --no-pager 2>/dev/null \
        | grep -E '比对结果|质量|候选|命中|验证' | tail -6
fi
