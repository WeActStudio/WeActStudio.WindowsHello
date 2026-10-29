#!/usr/bin/env bash
#
# fetch-libfprint.sh —— 重新下载 libfprint v1.94.100 源码到 vendor/
#
# 仓库里**已经自带** vendor/libfprint-1.94.100.tar.gz，正常安装不需要跑这个脚本。
# 只有两种情况需要跑它：①把包删了；②想从上游刷新一份（加 --force）。
#
#   ./scripts/fetch-libfprint.sh           # 包不存在才下载并打包
#   ./scripts/fetch-libfprint.sh --force   # 已存在也重新下载
#
# 产物格式要和 install.sh 的预期一致：
#   tar -xzf vendor/libfprint-1.94.100.tar.gz 之后里面是 libfprint-1.94.100/ 目录
#
# 为什么锁 1.94.100：驱动按 libfprint 1.94 系列的驱动 API 写，1.95+ 改过接口。
#
set -euo pipefail

VERSION="1.94.100"
TAG="v${VERSION}"
DIRNAME="libfprint-${VERSION}"
PKG_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENDOR="$PKG_ROOT/vendor"
OUT="$VENDOR/${DIRNAME}.tar.gz"
UPSTREAM="https://gitlab.freedesktop.org/libfprint/libfprint"

FORCE=0
for arg in "$@"; do
    case "$arg" in
        --force|-f) FORCE=1 ;;
        -h|--help)  sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "未知参数: $arg" >&2; exit 1 ;;
    esac
done

if [ -f "$OUT" ] && [ "$FORCE" -eq 0 ]; then
    echo "已存在: $OUT"
    echo "（想重新下载加 --force）"
    exit 0
fi

mkdir -p "$VENDOR"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "==> 获取 libfprint $TAG"

if command -v git >/dev/null 2>&1; then
    echo "    用 git clone（--depth 1 --branch $TAG）"
    git clone --depth 1 --branch "$TAG" "$UPSTREAM.git" "$TMP/$DIRNAME" \
        || { echo "git clone 失败，改用 tarball 下载" >&2; rm -rf "$TMP/$DIRNAME"; }
fi

if [ ! -d "$TMP/$DIRNAME" ]; then
    URL="$UPSTREAM/-/archive/$TAG/libfprint-$TAG.tar.gz"
    echo "    下载 $URL"
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 -o "$TMP/src.tar.gz" "$URL"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$TMP/src.tar.gz" "$URL"
    else
        echo "需要 git / curl / wget 之一" >&2
        exit 1
    fi
    tar xzf "$TMP/src.tar.gz" -C "$TMP"
    # 上游 archive 解压出来是 libfprint-1.94.100（GitLab）或 libfprint-v1.94.100（GitHub 镜像）
    if [ ! -d "$TMP/$DIRNAME" ]; then
        for cand in "$TMP/libfprint-$TAG" "$TMP/libfprint-$VERSION"; do
            if [ -d "$cand" ]; then
                mv "$cand" "$TMP/$DIRNAME"
                break
            fi
        done
    fi
fi

[ -f "$TMP/$DIRNAME/meson.build" ] || { echo "下载内容不像 libfprint 源码树" >&2; exit 1; }

GOTVER=$(sed -n "s/.*version: *'\([^']*\)'.*/\1/p" "$TMP/$DIRNAME/meson.build" | head -1)
echo "    源码版本: ${GOTVER:-未知}"

# 去掉 .git，别把版本库塞进包里
rm -rf "$TMP/$DIRNAME/.git"

# 兼容性修补：上游 v1.94.100 的 tests/meson.build 用单变量迭代 dict，
# meson >= 1.10 会直接报 "Foreach expects exactly 2 variables"。
PATCH_FILE="$TMP/$DIRNAME/tests/meson.build"
if [ -f "$PATCH_FILE" ] && grep -qE 'foreach driver_test *: *drivers_tests' "$PATCH_FILE"; then
    sed -i 's/foreach driver_test *: *drivers_tests/foreach driver_test, _args : drivers_tests/' "$PATCH_FILE"
    echo "    已修补 tests/meson.build（meson >= 1.10 兼容）"
fi

# 打包（在 TMP 里打包，保证顶层目录名是 $DIRNAME）
echo "==> 打包"
tar czf "$OUT" -C "$TMP" "$DIRNAME"

SIZE=$(du -h "$OUT" | cut -f1)
echo "==> 完成: $OUT ($SIZE)"
tar tzf "$OUT" | head -1
echo
echo "接下来: sudo ./scripts/install.sh        # 会用 vendor/ 里的源码，离线可编"
