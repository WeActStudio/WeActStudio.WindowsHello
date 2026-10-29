#!/usr/bin/env bash
#
# moc5159-fprint-driver —— 卸载脚本
#
#   sudo ./scripts/uninstall.sh            # 移除本驱动，恢复系统原状
#   sudo ./scripts/uninstall.sh --purge    # 额外删除构建目录与源码（/var/tmp/moc5159-build）
#
# 说明：
#   - local 模式（默认）：删掉 /usr/local 下安装的文件 + ld.so.conf 片段，发行版 libfprint 不受影响；
#   - system 模式：从 /var/lib/moc5159-driver/backup/ 还原被覆盖的发行版文件。
#
set -uo pipefail

PKG_ROOT=$(cd "$(dirname "$0")/.." && pwd)
STATE_DIR="/var/lib/moc5159-driver"
LDSO_CONF="/etc/ld.so.conf.d/00-moc5159-libfprint.conf"
PURGE=0

[ "${1:-}" = "--purge" ] && PURGE=1
[ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ] && { sed -n '2,12p' "$0"; exit 0; }

if [ -t 1 ]; then C_G=$'\033[32m'; C_Y=$'\033[33m'; C_B=$'\033[1m'; C_0=$'\033[0m'
else C_G=""; C_Y=""; C_B=""; C_0=""; fi
step() { printf '\n%s==> %s%s\n' "$C_B" "$*" "$C_0"; }
info() { printf '    %s\n' "$*"; }
ok()   { printf '    %s[OK]%s %s\n' "$C_G" "$C_0" "$*"; }
warn() { printf '    %s[!]%s %s\n' "$C_Y" "$C_0" "$*"; }
die()  { printf '\n[错误] %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "需要 root 权限，请用 sudo 运行"

step "读取安装记录"
[ -f "$STATE_DIR/meta" ] || die "找不到 $STATE_DIR/meta —— 本驱动似乎没有通过 install.sh 安装"
# shellcheck disable=SC1091
. "$STATE_DIR/meta"
info "版本 $VERSION / 模式 $MODE / 前缀 $PREFIX / 库目录 $LIBDIR"

step "停止 fprintd"
systemctl stop fprintd >/dev/null 2>&1 || true
ok "已停止（若原本是 D-Bus 按需启动，之后会自动恢复）"

step "移除驱动文件"
REMOVED=0
if [ -f "$STATE_DIR/backup-manifest.txt" ] && [ -d "$STATE_DIR/backup" ]; then
    # system 模式：先还原被覆盖的发行版文件
    while IFS= read -r rel; do
        [ -n "$rel" ] || continue
        if [ -f "$STATE_DIR/backup/$rel" ]; then
            mkdir -p "/$(dirname "$rel")"
            cp -a "$STATE_DIR/backup/$rel" "/$rel"
            REMOVED=$((REMOVED+1))
        fi
    done <"$STATE_DIR/backup-manifest.txt"
    ok "已还原 $REMOVED 个发行版原文件"
    info "（还原清单见 $STATE_DIR/backup-manifest.txt）"
fi

if [ -f "$STATE_DIR/manifest.txt" ]; then
    # 只删「不是上面已还原」的文件；用还原清单做排除
    DELETED=0
    while IFS= read -r rel; do
        [ -n "$rel" ] || continue
        [ -f "/$rel" ] || continue
        if [ -f "$STATE_DIR/backup/$rel" ] && [ -s "$STATE_DIR/backup/$rel" ]; then
            continue                       # 已还原的发行版文件，保留
        fi
        rm -f "/$rel" && DELETED=$((DELETED+1))
    done <"$STATE_DIR/manifest.txt"
    ok "已删除 $DELETED 个驱动文件"
else
    warn "没有安装清单，回退为按已知文件名删除"
    for pat in "$PREFIX"/lib*/libfprint-2.so.2* "$PREFIX"/lib*/libfprint-2.so; do
        [ -e "$pat" ] && rm -f "$pat" && ok "删除 $pat"
    done
    rm -rf "$PREFIX/include/fprint" 2>/dev/null
    rm -f "$PREFIX"/lib*/pkgconfig/libfprint-2.pc 2>/dev/null
fi

step "清理动态库配置"
if [ -f "$LDSO_CONF" ]; then
    rm -f "$LDSO_CONF"
    ok "已删除 $LDSO_CONF"
fi
ldconfig 2>/dev/null || true

if command -v apt-mark >/dev/null 2>&1; then
    apt-mark unhold libfprint-2-2 >/dev/null 2>&1 && ok "已解除 libfprint-2-2 的版本锁定"
fi

step "刷新服务"
systemctl try-restart fprintd >/dev/null 2>&1 || true

if [ "$PURGE" -eq 1 ]; then
    step "深度清理"
    rm -rf /var/tmp/moc5159-build && ok "已删除构建目录 /var/tmp/moc5159-build"
fi

rm -rf "$STATE_DIR"
ok "已删除安装记录 $STATE_DIR"

# 检查另一种模式是否留了孤儿文件（先装 local 再装 system 的典型情况）
LEFTOVER=""
for d in /usr/local/lib /usr/local/lib64 /usr/local/lib/*-linux-gnu*; do
    [ -e "$d/libfprint-2.so.2.0.0" ] && LEFTOVER="$LEFTOVER $d"
done
if [ -n "$LEFTOVER" ]; then
    warn "发现 /usr/local 下还留着另一份驱动:$LEFTOVER"
    info "清理: sudo rm -f $LEFTOVER/libfprint-2.so.2*  &&  sudo ldconfig"
fi
if [ -f "$LDSO_CONF" ]; then
    warn "动态库配置仍存在: $LDSO_CONF（内容: $(cat "$LDSO_CONF")）"
fi

cat <<EOF

${C_G}${C_B}卸载完成${C_0}

  local 模式：/usr/local 下的驱动文件已删除，发行版 libfprint 立即恢复生效。
  system 模式：被覆盖的文件已从备份还原。

验证：
  ldconfig -p | grep libfprint-2.so.2      # 应只剩发行版那一份
  systemctl restart fprintd && fprintd-list \$USER

注意：已登记的指纹数据（/var/lib/fprint/）不会被删除，需要的话手动清理：
  sudo rm -rf /var/lib/fprint/*/*/
EOF
