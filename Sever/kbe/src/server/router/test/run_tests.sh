#!/bin/sh
# ---------------------------------------------------------------------------
# Router 测试统一编排器 (Linux / macOS)
#
#   sh run_tests.sh [--with-router]
#
# 分层：
#   L1  纯逻辑单元（registry / protocol / mail）
#   L2  静态收敛检查（Windows PowerShell 脚本，Linux 下 SKIP）
#   L3  真实 router 进程协议测试（需要 ./router + proto client；--with-router 时用
#       assets/res/server/kbengine.xml 拉起 router 子进程并跑客户端，客户端退出码
#       2(router 不可达)按 SKIP 处理）
#   L4  集群 E2E（不在本脚本内，见 TESTCASES.md）
#
# 退出码：0=全部通过 1=有用例失败 2=构建失败
# ---------------------------------------------------------------------------

TESTDIR=$(cd "$(dirname "$0")" && pwd)
cd "$TESTDIR"

WITH_ROUTER=0
[ "$1" = "--with-router" ] && WITH_ROUTER=1

passed=0
failed=0
skipped=0

run_suite() {
    layer=$1
    exe=$2

    echo ""
    echo "########## $layer  $exe ##########"

    if [ ! -x "$TESTDIR/$exe" ]; then
        echo "[ SKIP ] $exe not built"
        skipped=$((skipped + 1))
        return
    fi

    "./$exe"
    if [ $? -eq 0 ]; then passed=$((passed + 1)); else failed=$((failed + 1)); fi
}

sh _build_tests.sh || exit 2

run_suite "L1" router_registry_test
run_suite "L1" router_protocol_test
run_suite "L1" router_mail_test

echo ""
echo "########## L2  static convergence check ##########"
echo "[ SKIP ] converge_static_check.ps1 is Windows/PowerShell only"
skipped=$((skipped + 1))

echo ""
echo "########## L3  router process protocol tests ##########"

# 与 assets/res/server/kbengine.xml 中的 <router> 段保持一致。
ROUTER_SERVER_PORT=20296
PENDING_BUFFER_MAX=8
PENDING_TIMEOUT_MS=2000

if [ "$WITH_ROUTER" -ne 1 ]; then
    echo "[ SKIP ] L3 requires --with-router"
elif [ ! -x "$TESTDIR/router" ]; then
    echo "[ SKIP ] ./router not found (build it first)"
elif [ ! -x "$TESTDIR/router_proto_client" ]; then
    echo "[ SKIP ] router_proto_client not built (run _build_tests.sh)"
else
    # 用 test/assets/res/server/kbengine.xml 覆盖本次 router 子进程的 <router> 段
    # (端口 / pending 上限 / 超时)，不污染产品配置。
    RUNDIR="$TESTDIR/run"
    mkdir -p "$RUNDIR"

    ASSETS="$TESTDIR/assets"
    KBE_SRC=$(cd "$TESTDIR/../../.." && pwd)
    KBE_ROOT=$(cd "$KBE_SRC/../.." && pwd)
    KBE_RES="$KBE_ROOT/kbe/res"

    KBE_BIN_PATH="$KBE_ROOT/kbe/bin/server/"
    KBE_RES_PATH="$KBE_RES:$ASSETS:$ASSETS/scripts:$ASSETS/res"
    export KBE_ROOT KBE_BIN_PATH KBE_RES_PATH

    echo "[ RUN  ] starting ./router (serverPort=$ROUTER_SERVER_PORT, pendingMax=$PENDING_BUFFER_MAX, pendingTimeoutMS=$PENDING_TIMEOUT_MS)"

    ./router --cid=99001 --gus=250 >"$RUNDIR/router.stdout.log" 2>"$RUNDIR/router.stderr.log" &
    ROUTER_PID=$!

    # 无论成败都要回收 router 子进程，避免残留占用端口
    trap 'if [ -n "$ROUTER_PID" ]; then kill "$ROUTER_PID" 2>/dev/null; fi' EXIT INT TERM

    ./router_proto_client --port="$ROUTER_SERVER_PORT" \
        --pending-max="$PENDING_BUFFER_MAX" --pending-timeout-ms="$PENDING_TIMEOUT_MS"
    CLIENT_RC=$?

    if [ -n "$ROUTER_PID" ]; then
        kill "$ROUTER_PID" 2>/dev/null
        wait "$ROUTER_PID" 2>/dev/null
        echo "         router stopped (pid was $ROUTER_PID); logs: run/router.stdout.log / run/router.stderr.log"
        ROUTER_PID=""
    fi

    if [ "$CLIENT_RC" -eq 2 ]; then
        # 退出码 2 = 环境不可用(router 没起来/端口不通)，属于 SKIP 而不是失败
        echo "[ SKIP ] router not reachable on port $ROUTER_SERVER_PORT (see run/router.stderr.log)"
        skipped=$((skipped + 1))
    elif [ "$CLIENT_RC" -eq 0 ]; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
    fi
fi

echo ""
echo "########## SUMMARY: $passed passed, $failed failed, $skipped skipped ##########"

[ $failed -gt 0 ] && exit 1
exit 0
