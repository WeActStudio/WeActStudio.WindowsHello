#!/usr/bin/env bash
#
# run-tests.sh — regression run of the whole simulator test matrix
#
#   ./run-tests.sh            # build, then run every scenario
#   ./run-tests.sh --quick    # skip the slow fault scenarios
#
# Exit code 0 means every scenario behaved as expected. This is the one command
# to run after touching the driver or the protocol layer.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$HERE/build"
PRELOAD="$BIN/libmocsim.so"
QUICK=0

[ "${1:-}" = "--quick" ] && QUICK=1

pass=0
fail=0
declare -a results

run_case () {
    local name="$1" expect="$2" ; shift 2
    local out rc

    out="$(timeout 120 env LD_PRELOAD="$PRELOAD" "$@" 2>&1)"
    rc=$?

    local got="fail"
    [ "$rc" -eq 0 ] && got="pass"

    if [ "$got" = "$expect" ]; then
        results+=("  ok   $name (rc=$rc)")
        pass=$((pass + 1))
    else
        results+=("  FAIL $name (rc=$rc, expected $expect)")
        fail=$((fail + 1))
        printf '%s\n' "$out" | tail -15 | sed 's/^/       /'
    fi
}

echo "==> 构建"
"$HERE/build.sh" >/dev/null || { echo "构建失败"; exit 1; }

echo "==> 协议自检"
if timeout 60 env LD_PRELOAD="$PRELOAD" "$BIN/mocsim-selftest" >/dev/null 2>&1; then
    results+=("  ok   mocsim-selftest")
    pass=$((pass + 1))
else
    results+=("  FAIL mocsim-selftest")
    fail=$((fail + 1))
    timeout 60 env LD_PRELOAD="$PRELOAD" "$BIN/mocsim-selftest" 2>&1 | tail -20 | sed 's/^/       /'
fi

if [ ! -x "$BIN/mocsim-e2e" ]; then
    echo "跳过端到端：mocsim-e2e 未构建（需要 libfprint-2）"
else
    echo "==> 端到端场景"

    run_case "默认：1 条模板，命中"          pass "$BIN/mocsim-e2e"
    run_case "未命中 (MOCSIM_MATCH=0)"       pass env MOCSIM_MATCH=0 "$BIN/mocsim-e2e"
    run_case "空库：完整登记流程"             pass env MOCSIM_TEMPLATES=0 "$BIN/mocsim-e2e"
    run_case "3 条模板"                      pass env MOCSIM_TEMPLATES=3 "$BIN/mocsim-e2e"

    if [ "$QUICK" -eq 0 ]; then
        run_case "故障：IN 侧残留帧"          pass env MOCSIM_TEMPLATES=0 MOCSIM_FAULT=stale "$BIN/mocsim-e2e"
        run_case "故障：缺 0xFF 尾帧"          pass env MOCSIM_TEMPLATES=0 MOCSIM_FAULT=notail "$BIN/mocsim-e2e"
        run_case "故障：首次采集质量不足"      pass env MOCSIM_TEMPLATES=0 MOCSIM_FAULT=quality "$BIN/mocsim-e2e"
        run_case "故障：C1 不应答"             fail env MOCSIM_FAULT=c1timeout "$BIN/mocsim-e2e"
        run_case "故障：应答校验和错误"        fail env MOCSIM_FAULT=badcrc "$BIN/mocsim-e2e"
        run_case "故障：设备不存在"            fail env MOCSIM_FAULT=gone "$BIN/mocsim-e2e"
    fi
fi

echo
printf '%s\n' "${results[@]}"
echo
printf '%d passed, %d failed\n' "$pass" "$fail"

[ "$fail" -eq 0 ]
