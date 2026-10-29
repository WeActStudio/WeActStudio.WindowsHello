#!/usr/bin/env bash
#
# moc5159-fprint-driver —— 环境自检 / 排错脚本
#
#   ./scripts/doctor.sh            # 普通用户即可（部分检查会提示需要 sudo）
#   sudo ./scripts/doctor.sh       # 完整检查（含设备访问）
#
# 只读，不修改任何东西。把输出整段贴出来就能定位大部分问题。
#
set -uo pipefail

PKG_ROOT=$(cd "$(dirname "$0")/.." && pwd)
STATE_DIR="/var/lib/moc5159-driver"

if [ -t 1 ]; then C_R=$'\033[31m'; C_G=$'\033[32m'; C_Y=$'\033[33m'; C_B=$'\033[1m'; C_0=$'\033[0m'
else C_R=""; C_G=""; C_Y=""; C_B=""; C_0=""; fi
head1() { printf '\n%s%s%s\n' "$C_B" "$*" "$C_0"; }
good() { printf '  %s[OK]%s   %s\n' "$C_G" "$C_0" "$*"; }
bad()  { printf '  %s[!!]%s   %s\n' "$C_R" "$C_0" "$*"; }
note() { printf '  %s[--]%s   %s\n' "$C_Y" "$C_0" "$*"; }

IS_ROOT=0
[ "$(id -u)" -eq 0 ] && IS_ROOT=1

head1 "1. 系统"
. /etc/os-release 2>/dev/null && note "发行版 : $PRETTY_NAME"
note "内核   : $(uname -r)  ($(uname -m))"
note "时间   : $(date -Iseconds)"

head1 "2. 设备"
if command -v lsusb >/dev/null 2>&1; then
    DEV=$(lsusb 2>/dev/null | grep -i '28e9:5159')
    if [ -n "$DEV" ]; then
        good "识别到指纹设备: $DEV"
        NODE=$(lsusb -d 28e9:5159 2>/dev/null | sed -n 's/^Bus \([0-9]*\) Device \([0-9]*\).*/\1 \2/p')
    else
        bad "没有找到 28e9:5159（检查是否插好 / 是否已从 Windows 直通过来）"
        note "如果是在虚拟机里，请确认 USB 设备已直通给客户机"
    fi
else
    note "缺少 lsusb（Debian: apt install usbutils）"
fi
if [ -d /sys/bus/usb/devices ]; then
    note "USB 端点:"
    FOUND_DEV=0
    for d in /sys/bus/usb/devices/*/idVendor; do
        [ -f "$d" ] || continue
        [ "$(cat "$d" 2>/dev/null)" = "28e9" ] || continue
        dev=$(dirname "$d")
        [ "$(cat "$dev/idProduct" 2>/dev/null)" = "5159" ] || continue
        FOUND_DEV=1
        EPS=$(ls -d "$dev"/ep_* 2>/dev/null | xargs -n1 basename 2>/dev/null | tr '\n' ' ')
        note "  $(basename "$dev"): 端点 [${EPS:-无}]"
    done
    [ "$FOUND_DEV" -eq 0 ] && note "  （没有 28e9:5159 节点）"
fi

head1 "3. libfprint 库"
if command -v ldconfig >/dev/null 2>&1; then
    ALL=$(ldconfig -p 2>/dev/null | grep 'libfprint-2\.so\.2 ' || true)
    if [ -z "$ALL" ]; then
        bad "系统里没有 libfprint-2.so.2"
    else
        FIRST=$(echo "$ALL" | head -1 | awk '{print $NF}')
        echo "$ALL" | awk '{print "         " $NF}'
        if echo "$FIRST" | grep -q '^/usr/local'; then
            good "首选库来自本驱动（$FIRST）"
        else
            case "$FIRST" in
                /usr/lib*|/lib*) note "首选库是发行版自带的: $FIRST" ;;
                *) note "首选库: $FIRST" ;;
            esac
        fi
    fi
fi
if [ -f "$STATE_DIR/meta" ]; then
    note "安装记录:"
    sed 's/^/         /' "$STATE_DIR/meta"
else
    note "没有找到 $STATE_DIR/meta（可能未用 install.sh 安装）"
fi

# 运行中的 fprintd 到底加载了哪一份
PID=$(pgrep -x fprintd | head -1)
if [ -n "$PID" ]; then
    LIB=$(grep -o '/[^ ]*libfprint-2\.so\.2[^ ]*' "/proc/$PID/maps" 2>/dev/null | head -1)
    if [ -n "$LIB" ]; then
        if echo "$LIB" | grep -q moc5159 || [ -f "$STATE_DIR/meta" ]; then
            good "fprintd 实际加载: $LIB"
        else
            note "fprintd 实际加载: $LIB"
        fi
    fi
else
    note "fprintd 进程未运行（D-Bus 按需启动，属正常）"
fi

head1 "4. 加载的 libfprint 里有没有 moc5159 驱动"
# 优先看 fprintd 是否已经把设备认出来（最直接的证据）
DEVNAME=""
if command -v fprintd-list >/dev/null 2>&1; then
    DEVNAME=$(fprintd-list "${SUDO_USER:-$(id -un)}" 2>&1 | grep -o 'Petaic MOC-5159[^)]*' | head -1 || true)
fi
if [ -n "$DEVNAME" ]; then
    good "fprintd 已识别设备: $DEVNAME"
else
    LOADED_SO=""
    PID=$(pgrep -x fprintd | head -1)
    if [ -n "$PID" ]; then
        LOADED_SO=$(grep -o '/[^ ]*libfprint-2\.so\.2[^ ]*' "/proc/$PID/maps" 2>/dev/null | head -1)
    fi
    [ -z "$LOADED_SO" ] && LOADED_SO=$(ldconfig -p 2>/dev/null | grep -m1 'libfprint-2\.so\.2 ' | awk '{print $NF}')
    if [ -n "$LOADED_SO" ] && [ -f "$LOADED_SO" ]; then
        # 注意用 grep -c 而不是 grep -q: set -o pipefail 下 grep -q 提前退出会让
        # strings 收到 SIGPIPE(141), 整条管道判为失败 -> 误报"库不含驱动"。
        HIT=$(strings "$LOADED_SO" 2>/dev/null | grep -c 'Petaic MOC-5159' || true)
        if [ "${HIT:-0}" -gt 0 ]; then
            note "库里含 moc5159 驱动（$LOADED_SO），但 fprintd 目前没列出设备"
            note "→ 设备没插好 / 被占用 / fprintd 没重启: sudo systemctl restart fprintd"
        else
            bad "当前加载的库不含 moc5159 驱动: $LOADED_SO"
            note "→ 检查 ldconfig -p 的顺序，或用 install.sh 重装"
        fi
    else
        note "拿不到已加载的 libfprint 路径"
    fi
fi

head1 "5. fprintd / 指纹数据"
if command -v fprintd-list >/dev/null 2>&1; then
    U="${SUDO_USER:-$(id -un)}"
    RAW=$(fprintd-list "$U" 2>&1)
    OUT=$(printf '%s\n' "$RAW" | grep -E 'finger|print' | tail -6 || true)
    if printf '%s\n' "$RAW" | grep -qi 'no prints'; then
        note "用户 $U 还没有登记指纹（sudo ./tools/fp-enroll.sh right-index-finger 右手食指）"
    elif [ -n "$OUT" ]; then
        good "用户 $U 已登记的指纹:"
        printf '%s\n' "$OUT" | sed 's/^/         /'
    else
        note "fprintd-list 输出:"
        printf '%s\n' "$RAW" | tail -5 | sed 's/^/         /'
    fi
else
    bad "没有 fprintd-list（Debian: apt install fprintd）"
fi
if [ "$IS_ROOT" -eq 1 ]; then
    STORE=$(ls -1 /var/lib/fprint/*/*/*/  2>/dev/null | wc -l)
    note "存储里共 $STORE 个指纹文件（/var/lib/fprint）"
fi

head1 "6. PAM（指纹登录 / 锁屏 / sudo）"
if grep -q 'pam_fprintd' /etc/pam.d/common-auth 2>/dev/null; then
    LINE=$(grep 'pam_fprintd' /etc/pam.d/common-auth | head -1)
    good "common-auth 已启用: $LINE"
    echo "$LINE" | grep -q 'max-tries' || note "没有设 max-tries，按错一次就会回落到密码"
elif grep -q 'pam_fprintd' /etc/pam.d/system-auth 2>/dev/null; then
    good "system-auth 已启用 pam_fprintd"
else
    note "PAM 里没有 pam_fprintd（只影响登录/锁屏；fprintd-* 命令仍可用）"
    note "Debian/Ubuntu: sudo pam-auth-update --enable fprintd"
fi

head1 "7. 设备通信（需要 root）"
PROBE=""
for p in "$PKG_ROOT/build/probe" "$(command -v moc5159-probe 2>/dev/null)" "./probe"; do
    [ -x "$p" ] && { PROBE="$p"; break; }
done
if [ -z "$PROBE" ]; then
    note "没有 probe（编译: sudo $PKG_ROOT/scripts/build-probe.sh）"
elif [ "$IS_ROOT" -ne 1 ]; then
    note "probe 需要 root：sudo $0"
else
    systemctl stop fprintd >/dev/null 2>&1 || true
    if OUT=$("$PROBE" list 2>&1); then
        if echo "$OUT" | grep -qi 'ev=0x26\|超时\|timeout'; then
            bad "probe list 异常，输出:"
            echo "$OUT" | tail -6 | sed 's/^/         /'
        else
            good "probe list 正常:"
            echo "$OUT" | grep -E '模板|记录|容量|已用|\[[0-9]\]' | head -8 | sed 's/^/         /'
        fi
    else
        bad "probe 打开设备失败（设备被占用 / 通道挂死，尝试拔插一次）"
    fi
    systemctl start fprintd >/dev/null 2>&1 || true
fi

head1 "8. 最近日志（fprintd 驱动调试输出）"
if command -v journalctl >/dev/null 2>&1; then
    L=$(journalctl -t fprintd --since '-30 min' --no-pager 2>/dev/null | tail -12)
    if [ -n "$L" ]; then echo "$L" | sed 's/^/  /'; else note "最近 30 分钟没有 fprintd 日志"; fi
else
    note "没有 journalctl"
fi

head1 "结论提示"
note "设备没出现 → 插好/直通；驱动没生效 → 看第 3、4 项；按了没反应 → 看第 7、8 项。"
note "多指只能用一根：这是芯片固件限制，见 docs/zh-CN/02-usage.md。"
echo
