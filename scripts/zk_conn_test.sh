#!/usr/bin/env bash
#
# zk_conn_test.sh —— 测量「server 进程到 ZooKeeper 的连接数」及相关指标
#
# 用途：验证「方案 A（合并 ZK 双连接）」改动前后的效果。
#       核心主张是「从 2 条连接变成 1 条」，所以这个脚本就是把它数出来。
#
# 用法：
#   ./scripts/zk_conn_test.sh            # 跑一次测量
#   ./scripts/zk_conn_test.sh --keep     # 测完不关 server（方便手工继续观察）
#
# 前提：ZooKeeper 已在 127.0.0.1:2181 监听，8989 端口空闲。
#
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$REPO/build"
LOG=/tmp/zktest/zk_conn_test.log
KEEP=0
[ "${1:-}" = "--keep" ] && KEEP=1

mkdir -p /tmp/zktest

# ── 前提检查 ──────────────────────────────────────────────
ss -tln 2>/dev/null | grep -q ':2181' || { echo "❌ ZK 未在 2181 监听"; exit 1; }
ss -tln 2>/dev/null | grep -q ':8989' && { echo "❌ 8989 已被占用"; exit 1; }
[ -x "$BUILD/server" ] || { echo "❌ 找不到 $BUILD/server，先构建"; exit 1; }

echo "════════════════════════════════════════════"
echo " 测量时间: $(date '+%F %T')"
echo " 二进制  : $BUILD/server"
echo "════════════════════════════════════════════"

# ── 起 server ─────────────────────────────────────────────
: > "$LOG"
"$BUILD/server" > "$LOG" 2>&1 &
SRV_PID=$!

# 等注册完成（最多 15 秒）
for _ in $(seq 1 30); do
  grep -q "Register success" "$LOG" 2>/dev/null && break
  sleep 0.5
done

if ! kill -0 "$SRV_PID" 2>/dev/null; then
  echo "❌ server 启动失败，日志尾部："; tail -15 "$LOG"; exit 1
fi

# ── 指标 1：TCP 连接数（核心）─────────────────────────────
CONNS=$(ss -tnp 2>/dev/null | grep ':2181' | grep -c "pid=$SRV_PID,")
echo
echo "【指标 1】server → ZK:2181 的 TCP 连接数"
echo "          期望：改动前 = 2，改动后 = 1"
echo "          实测：$CONNS"
ss -tnp 2>/dev/null | grep ':2181' | grep "pid=$SRV_PID," | awk '{printf "            %s  local=%s\n", $1, $4}'

# ── 指标 2：ZK 侧 session 数 + 各自最后一次操作 ────────────
echo
echo "【指标 2】ZK 侧看到的 session（lop = 最后一次操作类型）"
echo "          lop=CREA 的那条 = 注册用的连接（改动后应消失）"
if exec 3<>/dev/tcp/127.0.0.1/2181 2>/dev/null; then
  printf 'cons' >&3
  timeout 2 cat <&3 | grep -oE '/[0-9.]+:[0-9]+\[[0-9]\]\(.*lop=[A-Z]+' \
    | sed -E 's|/127\.0\.0\.1:([0-9]+)\[[0-9]\]\(.*lop=([A-Z]+)|            端口 \1  lop=\2|'
  exec 3<&- 2>/dev/null
else
  echo "            （无法连接 ZK 取 cons，跳过）"
fi

# ── 指标 3：注册耗时 ──────────────────────────────────────
echo
echo "【指标 3】注册路径耗时（zookeeper_init success → Register success）"
awk '
  /zookeeper_init success/ { t1=$2 }
  /Register success/       { t2=$2; print "           " t1 "  →  " t2 }
' "$LOG" | head -2
echo "          期望：改动前 ≈ 1.0s（CreateRegistry 里的 sleep_for(1s)），改动后 < 0.1s"

# ── 指标 4：功能回归 ──────────────────────────────────────
echo
echo "【指标 4】功能回归（client 端到端调用）"
if timeout 20 "$BUILD/client" > /tmp/zktest/zk_conn_client.log 2>&1; then
  if grep -q "RPC success" /tmp/zktest/zk_conn_client.log; then
    echo "          ✅ $(grep -o 'RPC success.*' /tmp/zktest/zk_conn_client.log)"
  else
    echo "          ❌ client 退出码 0 但没有 RPC success"
  fi
else
  echo "          ❌ client 失败，日志尾部："; tail -5 /tmp/zktest/zk_conn_client.log
fi

# ── 指标 5：节点与日志副作用 ──────────────────────────────
echo
echo "【指标 5】其它可观察项"
echo "          注册的节点    : $(grep -o '/rpc_mt/[^ ]*' "$LOG" | tail -1)"
if grep -q "zoo_set_debug_level\|ZOO_LOG_LEVEL" "$LOG"; then :; fi
ZOOINFO_AFTER=$(awk '/Register success/{f=1} f && /ZOO_INFO/{c++} END{print c+0}' "$LOG")
echo "          注册后 ZK 客户端 INFO 日志条数: $ZOOINFO_AFTER"
echo "            （改动前应为 0 —— ServiceRegistry 构造函数调了"
echo "             zoo_set_debug_level(WARN)，把全局日志级别压下去了）"

# ── 指标 6：关闭是否干净 ──────────────────────────────────
echo
echo "【指标 6】优雅关闭（SIGTERM）"
if [ "$KEEP" = "0" ]; then
  kill -TERM "$SRV_PID" 2>/dev/null
  wait "$SRV_PID" 2>/dev/null
  RC=$?
  echo "          退出码: $RC"
  if [ "$RC" = "0" ]; then
    echo "          ✅ 干净退出"
  elif [ "$RC" = "139" ]; then
    echo "          ❌ SIGSEGV —— 已知问题："
    echo "             ~ManagerCycle 里 Stop() 解引用已 reset 的 Socket*，"
    echo "             且 LOG_INFO 发生在 spdlog::shutdown() 之后"
  else
    echo "          ⚠️  非零退出码 $RC（128+n 表示被信号 n 杀死）"
  fi
  grep -q "ManagerCycle::~ManagerCycle" "$LOG" \
    && echo "          （析构日志已输出）" \
    || echo "          （'ManagerCycle::~ManagerCycle' 未出现在日志中 —— 佐证崩溃点在析构）"
else
  echo "          （--keep：server 仍在运行，pid=$SRV_PID）"
fi

echo
echo "════════════════════════════════════════════"
echo " 完整日志: $LOG"
echo "════════════════════════════════════════════"
