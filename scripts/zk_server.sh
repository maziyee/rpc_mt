#!/usr/bin/env bash
#
# zk_server.sh —— 起/停一个用于本地测试的 ZooKeeper 实例
#
# 用法：
#   ./scripts/zk_server.sh start     # 前台阻塞启动（Ctrl-C 停止）
#   ./scripts/zk_server.sh daemon    # 后台启动
#   ./scripts/zk_server.sh stop      # 停止后台实例
#   ./scripts/zk_server.sh status    # 查看状态
#
# 为什么不用 /usr/share/zookeeper/bin/zkServer.sh：
#   1) 它把 pid 文件和日志硬编码到 /var/lib/zookeeper、/var/log/zookeeper，
#      普通用户没有写权限（会报 "FAILED TO WRITE PID / Permission denied"）
#   2) zkEnv.sh 里 ZOO_LOG_DIR 是【硬赋值】，用环境变量覆盖无效
#   所以这里绕开启动脚本，直接用 java 起 QuorumPeerMain。
#
set -uo pipefail

ZK_BASE=${ZK_BASE:-/tmp/zktest}
ZK_PORT=${ZK_PORT:-2181}
DATA_DIR="$ZK_BASE/data"
LOG_DIR="$ZK_BASE/log"
PID_FILE="$ZK_BASE/zk.pid"

# 从 zkEnv.sh 抄的 classpath（ZK 3.4.x 的依赖集合）
ZK_CP="/etc/zookeeper/conf:/usr/share/java/jline.jar:/usr/share/java/log4j-1.2.jar"
ZK_CP="$ZK_CP:/usr/share/java/xercesImpl.jar:/usr/share/java/xmlParserAPIs.jar"
ZK_CP="$ZK_CP:/usr/share/java/netty.jar:/usr/share/java/slf4j-api.jar"
ZK_CP="$ZK_CP:/usr/share/java/slf4j-log4j12.jar:/usr/share/java/zookeeper.jar"

write_config() {
  mkdir -p "$DATA_DIR" "$LOG_DIR"
  cat > "$ZK_BASE/zoo.cfg" <<EOF
tickTime=2000
initLimit=10
syncLimit=5
dataDir=$DATA_DIR
clientPort=$ZK_PORT
maxClientCnxns=60
EOF
  # 首次启动需要初始化 datadir
  if [ ! -f "$DATA_DIR/myid" ] && [ ! -d "$DATA_DIR/version-2" ]; then
    echo "初始化 datadir（首次运行）..."
  fi
}

is_running() {
  [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE" 2>/dev/null)" 2>/dev/null
}

start_foreground() {
  write_config
  exec java -cp "$ZK_CP" \
       -Dzookeeper.log.dir="$LOG_DIR" \
       -Dzookeeper.root.logger=INFO,CONSOLE \
       org.apache.zookeeper.server.quorum.QuorumPeerMain "$ZK_BASE/zoo.cfg"
}

start_daemon() {
  if is_running; then
    echo "ZK 已在运行 (pid=$(cat "$PID_FILE"))"
    return 0
  fi
  write_config
  nohup java -cp "$ZK_CP" \
        -Dzookeeper.log.dir="$LOG_DIR" \
        -Dzookeeper.root.logger=INFO,CONSOLE \
        org.apache.zookeeper.server.quorum.QuorumPeerMain "$ZK_BASE/zoo.cfg" \
        > "$LOG_DIR/zk.out" 2>&1 &
  echo $! > "$PID_FILE"

  for _ in $(seq 1 20); do
    if ss -tln 2>/dev/null | grep -q ":$ZK_PORT "; then
      echo "✅ ZK 已启动 (pid=$(cat "$PID_FILE"), port=$ZK_PORT)"
      return 0
    fi
    sleep 0.5
  done
  echo "❌ ZK 启动超时，日志尾部："
  tail -10 "$LOG_DIR/zk.out"
  return 1
}

stop_zk() {
  if is_running; then
    kill "$(cat "$PID_FILE")" 2>/dev/null
    sleep 2
    echo "✅ ZK 已停止"
  else
    echo "ZK 未在运行（顺带清理游离进程）"
    pkill -f QuorumPeerMain 2>/dev/null
  fi
  rm -f "$PID_FILE"
}

status_zk() {
  if is_running; then
    echo "运行中 (pid=$(cat "$PID_FILE"))"
  else
    echo "未运行"
  fi
  ss -tln 2>/dev/null | grep ":$ZK_PORT " || echo "  端口 $ZK_PORT 未监听"
}

case "${1:-daemon}" in
  start)  start_foreground ;;
  daemon) start_daemon ;;
  stop)   stop_zk ;;
  status) status_zk ;;
  *) echo "用法: $0 {start|daemon|stop|status}"; exit 1 ;;
esac
