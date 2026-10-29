#!/usr/bin/env bash
#
# fp-prompt.sh —— 指纹脚本共用的桌面提示框（zenity），用完立刻关。
#
# 用法（source 进来后用）：
#   . "$(dirname "$0")/fp-prompt.sh"
#   prompt "标题" "正文" [超时秒数]
#   prompt_stop            # 立刻关掉当前提示框（脚本退出前必须调用）
#
# 说明：zenity 的 --timeout 只是"最晚自己消失"，操作提前完成时必须显式 kill，
#       否则提示框会一直挂在桌面上挡事 —— 这是之前踩过的坑，统一在这里处理。
#
FP_PROMPT_PID=""
FP_PROMPT_TAG="指纹"

fp_detect_desktop() {
    if [ -z "${DISPLAY:-}" ]; then
        export DISPLAY=:0
    fi
    if [ -z "${XAUTHORITY:-}" ]; then
        local u="${SUDO_USER:-$USER}"
        if [ -f "/home/$u/.Xauthority" ]; then
            XAUTHORITY="/home/$u/.Xauthority"
        elif [ -f "/run/user/$(id -u "$u" 2>/dev/null)/gdm/Xauthority" ]; then
            XAUTHORITY="/run/user/$(id -u "$u")/gdm/Xauthority"
        else
            XAUTHORITY="$HOME/.Xauthority"
        fi
        export XAUTHORITY
    fi
}

fp_have_zenity() {
    command -v zenity >/dev/null 2>&1 && [ -n "${DISPLAY:-}" ]
}

prompt() {
    local title="$1" text="$2" tmo="${3:-120}"
    prompt_stop
    fp_have_zenity || { printf '%s\n' "$text"; return 0; }
    fp_detect_desktop
    zenity --info --title="$FP_PROMPT_TAG · $title" --text="$text" \
           --timeout="$tmo" --width=380 >/dev/null 2>&1 &
    FP_PROMPT_PID=$!
}

prompt_stop() {
    if [ -n "$FP_PROMPT_PID" ]; then
        kill "$FP_PROMPT_PID" 2>/dev/null
        wait "$FP_PROMPT_PID" 2>/dev/null
        FP_PROMPT_PID=""
    fi
    pkill -f "zenity --info --title=$FP_PROMPT_TAG" 2>/dev/null
    return 0
}
