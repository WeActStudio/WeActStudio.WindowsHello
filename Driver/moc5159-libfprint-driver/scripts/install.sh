#!/usr/bin/env bash
#
# moc5159-fprint-driver —— 安装脚本（多发行版通用）
#
#   sudo ./scripts/install.sh                 # 默认：装到 /usr/local，不覆盖发行版文件
#   sudo ./scripts/install.sh --system        # 装到 /usr，覆盖发行版 libfprint（会备份）
#   sudo ./scripts/install.sh --with-pam      # 安装后顺便把 pam_fprintd 接到登录/锁屏
#
# 设计要点：
#   1. 用 libfprint 1.94.100 源码：vendor/ 有就本地解压（离线），没有就 git clone 上游；
#   2. 默认 prefix=/usr/local + ld.so.conf.d 覆盖，**不动发行版包管理器的文件**，
#      卸载即删目录，升级发行版不会冲突；
#   3. 编译时默认构建 libfprint 全部驱动（281 个设备），不会让你机器上其它
#      指纹传感器失效；若缺依赖导致全量构建失败，自动回落到只编 moc5159。
#
set -uo pipefail

PKG_ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(cat "$PKG_ROOT/VERSION" 2>/dev/null || echo "unknown")

PREFIX="/usr/local"
MODE="local"                 # local | system
DRIVERS="all"                # all | moc5159
LFP_SRC=""
WORKDIR="/var/tmp/moc5159-build"
DO_DEPS=1
WITH_PAM=0
KEEP_BUILD=0

STATE_DIR="/var/lib/moc5159-driver"
LDSO_CONF="/etc/ld.so.conf.d/00-moc5159-libfprint.conf"

#--------------------------------------------------------------- 输出
if [ -t 1 ]; then
    C_R=$'\033[31m'; C_G=$'\033[32m'; C_Y=$'\033[33m'; C_B=$'\033[1m'; C_0=$'\033[0m'
else
    C_R=""; C_G=""; C_Y=""; C_B=""; C_0=""
fi
step() { printf '\n%s==> %s%s\n' "$C_B" "$*" "$C_0"; }
info() { printf '    %s\n' "$*"; }
ok()   { printf '    %s[OK]%s %s\n' "$C_G" "$C_0" "$*"; }
warn() { printf '    %s[!]%s %s\n' "$C_Y" "$C_0" "$*"; }
die()  { printf '\n%s[错误]%s %s\n' "$C_R" "$C_0" "$*" >&2; exit 1; }

usage() {
    cat <<EOF
moc5159-fprint-driver $VERSION 安装脚本

用法: sudo $0 [选项]

  --prefix DIR          安装前缀（默认 /usr/local；--system 时固定 /usr）
  --system              安装到 /usr，覆盖发行版 libfprint-2.so.2（会先备份，可完整还原）
  --drivers all|moc5159 构建哪些驱动。all=全部（默认，不影响其它指纹设备）；
                        moc5159=只编本驱动（体积小、依赖少）
  --libfprint-src DIR   使用指定的 libfprint 源码树（默认用 vendor/，没有则联网获取）
  --with-pam            安装后启用 pam_fprintd（Debian/Ubuntu 系用 pam-auth-update）
  --no-deps             不自动安装构建依赖
  --keep-build          保留 $WORKDIR 里的源码与构建目录
  -h, --help            显示本帮助

示例:
  sudo $0                        # 最常用：装到 /usr/local
  sudo $0 --system --with-pam    # 完全替换发行版 libfprint，并接上指纹登录
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)         PREFIX="${2:?}"; shift 2 ;;
        --prefix=*)       PREFIX="${1#*=}"; shift ;;
        --system)         MODE="system"; PREFIX="/usr"; shift ;;
        --drivers)        DRIVERS="${2:?}"; shift 2 ;;
        --drivers=*)      DRIVERS="${1#*=}"; shift ;;
        --libfprint-src)  LFP_SRC="${2:?}"; shift 2 ;;
        --libfprint-src=*) LFP_SRC="${1#*=}"; shift ;;
        --with-pam)       WITH_PAM=1; shift ;;
        --no-deps)        DO_DEPS=0; shift ;;
        --keep-build)     KEEP_BUILD=1; shift ;;
        -h|--help)        usage; exit 0 ;;
        *) die "未知参数: $1（用 --help 查看用法）" ;;
    esac
done

case "$DRIVERS" in all|moc5159) ;; *) die "--drivers 只能是 all 或 moc5159" ;; esac
[ "$MODE" = "system" ] && [ "$PREFIX" != "/usr" ] && die "--system 与 --prefix 冲突"

#--------------------------------------------------------------- 0. 前置检查
step "环境检查"

[ "$(id -u)" -eq 0 ] || die "需要 root 权限，请用 sudo 运行"

info "发行版 : $(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME" || echo "未知")"
info "内核   : $(uname -r)"
info "架构   : $(uname -m)"
info "安装模式: $([ "$MODE" = system ] && echo "system（覆盖 /usr 下的发行版 libfprint）" || echo "local（装到 $PREFIX，不覆盖发行版）")"

# 已装过其它模式时，为避免两份库互相打脸，要求先卸载
if [ -f "$STATE_DIR/meta" ]; then
    OLD_MODE=$(sed -n 's/^MODE=//p' "$STATE_DIR/meta" | head -1)
    OLD_PREFIX=$(sed -n 's/^PREFIX=//p' "$STATE_DIR/meta" | head -1)
    if [ "$OLD_MODE" != "$MODE" ]; then
        die "检测到已安装（模式 $OLD_MODE，前缀 $OLD_PREFIX），与本次（$MODE / $PREFIX）不同。
     请先执行: sudo $PKG_ROOT/scripts/uninstall.sh
     然后重新运行本脚本。"
    fi
    info "检测到同模式的已有安装（$OLD_MODE / $OLD_PREFIX），将覆盖升级"
fi

for f in src/moc5159.c src/moc5159.h src/moc5159-proto.c src/moc5159-proto.h \
         scripts/integrate.sh tools/moc5159-probe.c; do
    [ -f "$PKG_ROOT/$f" ] || die "安装包不完整，缺少 $f"
done
ok "安装包完整（版本 $VERSION）"

# 注意：这里刻意不用 `cmd | grep -q` —— 在 set -o pipefail 下 grep -q 提前退出会让
# 上游收到 SIGPIPE（exit 141），整条管道被判为失败，是经典的误判来源。
HAS_DEV=0
if command -v lsusb >/dev/null 2>&1; then
    USB_LIST=$(lsusb 2>/dev/null || true)
    case "$USB_LIST" in *28e9:5159*) HAS_DEV=1 ;; esac
fi
if [ "$HAS_DEV" -eq 1 ]; then
    DEV_LINE=$(printf '%s\n' "$USB_LIST" | grep -i '28e9:5159' || true)
    ok "检测到设备: $DEV_LINE"
else
    warn "当前没有检测到 28e9:5159 设备（可以继续安装，插上后再验证）"
fi

#--------------------------------------------------------------- 1. 依赖
PM=""
detect_pm() {
    for p in apt-get dnf yum zypper pacman apk tdnf; do
        command -v "$p" >/dev/null 2>&1 && { PM="$p"; return; }
    done
}
detect_pm

pm_install() {
    case "$PM" in
        apt-get) DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "$@" ;;
        dnf|yum|tdnf) "$PM" install -y "$@" ;;
        zypper)  zypper --non-interactive install --no-recommends "$@" ;;
        pacman)  pacman -S --needed --noconfirm "$@" ;;
        apk)     apk add --no-cache "$@" ;;
        *) return 1 ;;
    esac
}

deps_list() {
    case "$PM" in
        apt-get) echo "build-essential meson ninja-build pkg-config python3 git curl \
                       libglib2.0-dev libgusb-dev libudev-dev libssl-dev libusb-1.0-0-dev" ;;
        dnf|yum|tdnf) echo "gcc gcc-c++ make meson ninja-build pkgconf-pkg-config python3 git curl \
                       glib2-devel libgusb-devel systemd-devel openssl-devel libusb1-devel" ;;
        zypper) echo "gcc gcc-c++ make meson ninja pkg-config python3 git curl \
                      glib2-devel libgusb-devel systemd-devel libopenssl-devel libusb-1_0-devel" ;;
        pacman) echo "base-devel meson ninja pkgconf python git curl \
                      glib2 libgusb systemd openssl libusb" ;;
        apk) echo "build-base meson ninja pkgconf python3 git curl \
                   glib-dev libgusb-dev eudev-dev openssl-dev libusb-dev" ;;
        *) echo "" ;;
    esac
}

if [ "$DO_DEPS" -eq 1 ] && [ -n "$PM" ]; then
    step "安装构建依赖（$PM）"
    # shellcheck disable=SC2046
    [ "$PM" = "apt-get" ] && apt-get update -qq >/dev/null 2>&1 || true
    # shellcheck disable=SC2046
    if pm_install $(deps_list) >/tmp/moc5159-deps.log 2>&1; then
        ok "依赖已就绪"
    else
        warn "依赖安装有失败项，详见 /tmp/moc5159-deps.log（继续尝试构建）"
    fi
elif [ "$DO_DEPS" -eq 0 ]; then
    info "跳过依赖安装（--no-deps）"
else
    warn "未识别包管理器，请自行确保已安装 meson/ninja/gcc/glib/gusb/libudev/openssl 开发包"
fi

# meson 版本要求 >= 0.62
MESON_MIN="0.62"
ver_ge() { [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" = "$2" ]; }
if command -v meson >/dev/null 2>&1 && ver_ge "$(meson --version)" "$MESON_MIN"; then
    ok "meson $(meson --version)"
else
    warn "系统 meson 版本过低或缺失（需要 >= $MESON_MIN），尝试用 pip 安装"
    if command -v pip3 >/dev/null 2>&1; then
        pip3 install --break-system-packages "meson>=1.0" >/dev/null 2>&1 \
            || pip3 install "meson>=1.0" >/dev/null 2>&1 || true
    fi
    command -v meson >/dev/null 2>&1 \
        || die "无法获得 meson >= $MESON_MIN，请手动安装（如 pipx install meson）后重试"
    ok "meson $(meson --version)"
fi

command -v ninja >/dev/null 2>&1 || die "缺少 ninja"
command -v pkg-config >/dev/null 2>&1 || die "缺少 pkg-config"
for pc in glib-2.0 gio-unix-2.0 gusb; do
    pkg-config --exists "$pc" || die "缺少开发包: $pc（Debian: libglib2.0-dev/libgusb-dev）"
done
pkg-config --exists libudev || die "缺少开发包: libudev"
ok "编译环境检查通过"

#--------------------------------------------------------------- 2. 准备 libfprint 源码
step "准备 libfprint 源码"

SRC_BASE="$WORKDIR/src"
mkdir -p "$SRC_BASE"

if [ -n "$LFP_SRC" ]; then
    [ -f "$LFP_SRC/meson.build" ] || die "--libfprint-src 指向的不是 libfprint 源码树: $LFP_SRC"
    LFP_VERSION=$(sed -n "s/.*version: *'\([^']*\)'.*/\1/p" "$LFP_SRC/meson.build" | head -1)
    info "使用指定源码树: $LFP_SRC (版本 $LFP_VERSION)"
    if ! ver_ge "$LFP_VERSION" "1.94"; then
        warn "该源码树版本低于 1.94，驱动可能编译不过（建议用 scripts/fetch-libfprint.sh 取 1.94.100）"
    fi
    rm -rf "$SRC_BASE/libfprint-custom"
    cp -a "$LFP_SRC" "$SRC_BASE/libfprint-custom"
    LFP="$SRC_BASE/libfprint-custom"
elif [ -f "$PKG_ROOT/vendor/libfprint-1.94.100.tar.gz" ]; then
    info "使用本地源码: vendor/libfprint-1.94.100.tar.gz（离线可用）"
    rm -rf "$SRC_BASE/libfprint-1.94.100"
    tar -xzf "$PKG_ROOT/vendor/libfprint-1.94.100.tar.gz" -C "$SRC_BASE" \
        || die "解压 vendor/libfprint-1.94.100.tar.gz 失败"
    LFP="$SRC_BASE/libfprint-1.94.100"
else
    warn "没有 vendor 源码，尝试从网络获取 libfprint v1.94.100（离线环境先跑 scripts/fetch-libfprint.sh）"
    command -v git >/dev/null 2>&1 || die "缺少 git，且包内无 vendor 源码"
    rm -rf "$SRC_BASE/libfprint-1.94.100"
    git clone --depth 1 --branch v1.94.100 \
        https://gitlab.freedesktop.org/libfprint/libfprint.git \
        "$SRC_BASE/libfprint-1.94.100" 2>/dev/null \
        || die "下载 libfprint 源码失败（离线环境先跑 ./scripts/fetch-libfprint.sh）"
    LFP="$SRC_BASE/libfprint-1.94.100"
fi
ok "源码树: $LFP"

#--------------------------------------------------------------- 2.5 兼容性修补
# 上游 v1.94.100 的 tests/meson.build 在 meson >= 1.10 下会配置失败：
#   ERROR: Foreach expects exactly 2 variables for iterating over objects of type dict
# 原因是它用单个变量迭代 dict（drivers_tests）。补上第二个变量即可，只影响测试用例。
PATCH_FILE="$LFP/tests/meson.build"
if [ -f "$PATCH_FILE" ] && grep -qE 'foreach driver_test *: *drivers_tests' "$PATCH_FILE"; then
    sed -i 's/foreach driver_test *: *drivers_tests/foreach driver_test, _args : drivers_tests/' "$PATCH_FILE"
    info "已修补 tests/meson.build（适配 meson >= 1.10 的 dict 迭代语法）"
fi

#--------------------------------------------------------------- 3. 注册驱动
step "把 moc5159 驱动注册进 libfprint 构建系统"
bash "$PKG_ROOT/scripts/integrate.sh" "$LFP" || die "integrate.sh 执行失败"
grep -q "moc5159" "$LFP/libfprint/meson.build" || die "驱动注册失败"
ok "驱动已注册到 $LFP/libfprint/drivers/moc5159"

#--------------------------------------------------------------- 4. 编译
step "编译 libfprint + moc5159 驱动（首次编译约 1~3 分钟）"

BUILD="$WORKDIR/build"
rm -rf "$BUILD"

COMMON_OPTS=(
    "-Dprefix=$PREFIX"
    "-Dintrospection=false"
    "-Dudev_rules=disabled"
    "-Dudev_hwdb=disabled"
    "-Ddoc=false"
    "-Dinstalled-tests=false"      # 不装 libexec/installed-tests 那套测试钩子
)

meson_setup() {
    # $1 = drivers 参数（空 = 全部驱动）
    if [ -n "$1" ]; then
        meson setup "$BUILD" "$LFP" "${COMMON_OPTS[@]}" "$1"
    else
        meson setup "$BUILD" "$LFP" "${COMMON_OPTS[@]}"
    fi
}

SETUP_LOG="$WORKDIR/meson-setup.log"
mkdir -p "$WORKDIR"

if [ "$DRIVERS" = "all" ]; then
    info "构建全部驱动（不会让机器上其它指纹设备失效）"
    if ! meson_setup "" >"$SETUP_LOG" 2>&1; then
        warn "全量构建配置失败，回落到只编 moc5159"
        info "（原因通常是缺 openssl 开发包；详见 $SETUP_LOG）"
        rm -rf "$BUILD"
        [ -n "$PM" ] && [ "$DO_DEPS" -eq 1 ] && pm_install libssl-dev >/dev/null 2>&1 || true
        if meson_setup "-Ddrivers=moc5159" >"$SETUP_LOG" 2>&1; then
            DRIVERS="moc5159"
        else
            tail -25 "$SETUP_LOG"
            die "meson 配置失败，日志: $SETUP_LOG"
        fi
    fi
else
    info "只构建 moc5159 驱动"
    meson_setup "-Ddrivers=moc5159" >"$SETUP_LOG" 2>&1 || { tail -25 "$SETUP_LOG"; die "meson 配置失败"; }
fi
ok "meson 配置完成（drivers=$DRIVERS）"

ninja -C "$BUILD" >"$WORKDIR/ninja.log" 2>&1 || { tail -30 "$WORKDIR/ninja.log"; die "编译失败，日志: $WORKDIR/ninja.log"; }
ok "编译完成"

SUPPORTED="$BUILD/libfprint/fprint-list-supported-devices"
[ -x "$SUPPORTED" ] || die "没有生成 $SUPPORTED"
SUP_OUT=$(printf '%s\n' "$("$SUPPORTED" 2>/dev/null)" || true)
case "$SUP_OUT" in
    *28e9:5159*)
        ok "驱动已生成，设备表包含: $(printf '%s\n' "$SUP_OUT" | grep -i '28e9:5159' || true)"
        info "共支持 $(printf '%s\n' "$SUP_OUT" | grep -c '|' || true) 个设备"
        ;;
    *)
        die "编译产物里没有 28e9:5159，驱动注册有问题"
        ;;
esac

#--------------------------------------------------------------- 5. 安装（先 staging，便于精确卸载）
step "安装到 $PREFIX"

STAGE="$WORKDIR/stage"
rm -rf "$STAGE"
DESTDIR="$STAGE" ninja -C "$BUILD" install >"$WORKDIR/install.log" 2>&1 \
    || { tail -20 "$WORKDIR/install.log"; die "安装到 staging 失败"; }

# 找出实际的 libdir（Debian 系 lib/x86_64-linux-gnu，Fedora 系 lib64）
SO_FILE=$(find "$STAGE" -name 'libfprint-2.so.2.0.0' -type f | head -1)
[ -n "$SO_FILE" ] || die "staging 里找不到 libfprint-2.so.2.0.0"
LIBDIR=$(dirname "${SO_FILE#"$STAGE"}")
info "库目录: $LIBDIR"

mkdir -p "$STATE_DIR"
MANIFEST="$STATE_DIR/manifest.txt"
( cd "$STAGE" && find . \( -type f -o -type l \) | sed 's|^\./||' | sort ) >"$MANIFEST"
info "安装清单: $(wc -l <"$MANIFEST") 个文件"

BACKUP="$STATE_DIR/backup"
if [ "$MODE" = "system" ]; then
    info "备份将被覆盖的发行版文件 -> $BACKUP"
    rm -rf "$BACKUP"; mkdir -p "$BACKUP"
    : >"$STATE_DIR/backup-manifest.txt"
    while IFS= read -r rel; do
        if [ -f "/$rel" ] && [ ! -L "/$rel" ]; then
            mkdir -p "$BACKUP/$(dirname "$rel")"
            cp -a "/$rel" "$BACKUP/$rel"
            echo "$rel" >>"$STATE_DIR/backup-manifest.txt"
        fi
    done <"$MANIFEST"
    info "已备份 $(wc -l <"$STATE_DIR/backup-manifest.txt") 个文件"
fi

cp -a "$STAGE/." / || die "复制文件到 / 失败"
ok "文件已安装"

# 记录安装元信息，供卸载使用
cat >"$STATE_DIR/meta" <<EOF
VERSION=$VERSION
MODE=$MODE
PREFIX=$PREFIX
LIBDIR=$LIBDIR
DRIVERS=$DRIVERS
INSTALLED_AT=$(date -Iseconds)
EOF

#--------------------------------------------------------------- 6. 让动态链接器优先用我们的库
step "配置动态库搜索路径"

if [ "$MODE" = "local" ]; then
    echo "$LIBDIR" >"$LDSO_CONF"
    ldconfig
    FIRST=$(ldconfig -p 2>/dev/null | grep -m1 'libfprint-2\.so\.2 ' | awk '{print $NF}')
    if [ "$FIRST" = "$LIBDIR/libfprint-2.so.2" ]; then
        ok "ldconfig 已优先解析到 $LIBDIR"
    else
        warn "ldconfig 首选路径是 $FIRST（不是 $LIBDIR）"
        warn "若功能异常，请检查 /etc/ld.so.conf.d/ 下的其它配置是否覆盖了本驱动"
    fi
else
    ldconfig
    if [ -n "$PM" ] && [ "$PM" = "apt-get" ]; then
        apt-mark hold libfprint-2-2 >/dev/null 2>&1 && ok "已 apt-mark hold libfprint-2-2（防止升级覆盖）"
    fi
    ok "已安装到 /usr 并刷新 ldconfig（原文件已备份，可 --system 卸载还原）"
fi

#--------------------------------------------------------------- 7. 重启 fprintd
step "重启指纹服务"

if command -v fprintd-list >/dev/null 2>&1 || systemctl cat fprintd >/dev/null 2>&1; then
    systemctl try-restart fprintd >/dev/null 2>&1 || true
    sleep 1
    if systemctl is-active --quiet fprintd; then
        ok "fprintd 正在运行"
    else
        info "fprintd 当前未运行（由 D-Bus 按需启动，属正常）"
    fi
else
    warn "系统未安装 fprintd（请先安装：apt install fprintd libpam-fprintd）"
fi

#--------------------------------------------------------------- 8. PAM（可选）
if [ "$WITH_PAM" -eq 1 ]; then
    step "启用 pam_fprintd（指纹登录/锁屏/sudo）"
    if command -v pam-auth-update >/dev/null 2>&1; then
        pam-auth-update --enable fprintd >/dev/null 2>&1 \
            && ok "已在 /etc/pam.d/common-auth 启用 pam_fprintd" \
            || warn "pam-auth-update 失败，请手动运行: sudo pam-auth-update --enable fprintd"
        if grep -q 'pam_fprintd.so' /etc/pam.d/common-auth 2>/dev/null; then
            if ! grep -q 'max-tries' /etc/pam.d/common-auth; then
                warn "默认 max-tries 可能为 1（按错一次就要输密码）"
                info "想给 5 次机会: sudo sed -i 's/pam_fprintd.so/pam_fprintd.so max-tries=5 timeout=10/' /etc/pam.d/common-auth"
            fi
        fi
    else
        warn "本发行版没有 pam-auth-update，请手动在 /etc/pam.d/system-auth 里加入:"
        info "auth  sufficient  pam_fprintd.so max-tries=5 timeout=10"
    fi
fi

#--------------------------------------------------------------- 9. 收尾
if [ "$KEEP_BUILD" -eq 0 ]; then
    rm -rf "$STAGE"
    info "已清理 staging 目录（源码与构建目录保留在 $WORKDIR，方便复编译）"
fi

cat <<EOF

${C_G}${C_B}安装完成（$VERSION）${C_0}

  驱动文件 : $LIBDIR/libfprint-2.so.2.0.0
  安装记录 : $STATE_DIR/meta
  设备表   : $SUPPORTED

接下来（需要真手指）：
  sudo fprintd-enroll $([ -n "${SUDO_USER:-}" ] && echo "${SUDO_USER}" || echo "<用户名>")
  fprintd-verify $([ -n "${SUDO_USER:-}" ] && echo "${SUDO_USER}" || echo "<用户名>")
  fprintd-list   $([ -n "${SUDO_USER:-}" ] && echo "${SUDO_USER}" || echo "<用户名>")

  桌面上一键登记（带按压提示、完事自动关窗）：
    sudo $PKG_ROOT/tools/fp-enroll.sh right-index-finger 右手食指

排错：
  sudo $PKG_ROOT/scripts/doctor.sh          # 环境自检
  sudo $PKG_ROOT/scripts/build-probe.sh     # 编译独立诊断工具 probe

${C_Y}重要：本设备固件只比对库内“第一条记录”，所以只登记 1 根手指才稳。${C_0}
详见 docs/zh-CN/02-usage.md 的“多手指的限制”一节。

卸载：sudo $PKG_ROOT/scripts/uninstall.sh
EOF
