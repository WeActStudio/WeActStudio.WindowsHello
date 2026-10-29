#!/bin/sh
# 把 moc5159 驱动装进一份 libfprint 源码树, 并注册到构建系统里。
#
# 用法:
#   ./scripts/integrate.sh ~/libfprint
#   meson setup ~/libfprint/build -Ddrivers=moc5159 -Dintrospection=false
#   ninja -C ~/libfprint/build
#
# 说明: libfprint 没有「外部插件」机制, 驱动必须编译进库内,
#       所以这里做两处注册 (驱动源文件表 + 驱动信息表), 都是幂等操作。

set -e

LFP=${1:?用法: $0 /path/to/libfprint}
HERE=$(cd "$(dirname "$0")/.." && pwd)

if [ ! -f "$LFP/libfprint/meson.build" ] || [ ! -f "$LFP/meson.build" ]; then
    echo "错误: $LFP 看起来不是 libfprint 源码树" >&2
    exit 1
fi

DST="$LFP/libfprint/drivers/moc5159"
mkdir -p "$DST"
cp "$HERE/src/moc5159.c" \
   "$HERE/src/moc5159.h" \
   "$HERE/src/moc5159-proto.c" \
   "$HERE/src/moc5159-proto.h" \
   "$DST/"
echo "已复制源码 -> $DST"

python3 - "$LFP" <<'PY'
import io, sys, re

lfp = sys.argv[1]

# 1) 驱动源文件表: libfprint/meson.build 的 driver_sources = { ... }
p = lfp + "/libfprint/meson.build"
s = open(p, encoding="utf-8").read()
if "'moc5159'" not in s:
    entry = ("    'moc5159' : files(\n"
             "        'drivers/moc5159/moc5159.c',\n"
             "        'drivers/moc5159/moc5159-proto.c',\n"
             "    ),\n")
    # 插到 driver_sources 表的最后一项之后 (表以 "}" 结束)
    m = re.search(r"driver_sources = \{(.*?)\n\}", s, re.S)
    if not m:
        sys.exit("找不到 driver_sources 表")
    s = s[:m.end() - 2] + entry + s[m.end() - 2:]
    open(p, "w", encoding="utf-8").write(s)
    print("已注册 driver_sources ->", p)
else:
    print("driver_sources 已包含 moc5159, 跳过")

# 2) 驱动信息表: 顶层 meson.build 的 drivers_info = { ... }
p = lfp + "/meson.build"
s = open(p, encoding="utf-8").read()
if "'moc5159'" not in s:
    m = re.search(r"drivers_info = \{(.*?)\n\}", s, re.S)
    if not m:
        sys.exit("找不到 drivers_info 表")
    s = s[:m.end() - 2] + "    'moc5159': {},\n" + s[m.end() - 2:]
    open(p, "w", encoding="utf-8").write(s)
    print("已注册 drivers_info ->", p)
else:
    print("drivers_info 已包含 moc5159, 跳过")
PY

echo
echo "完成。接下来:"
echo "  meson setup $LFP/build -Ddrivers=moc5159 -Dintrospection=false -Dudev_rules=disabled"
echo "  ninja -C $LFP/build"
