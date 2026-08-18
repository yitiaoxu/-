#!/bin/bash
# 板端内存监控：观察 vision_qt_demo 与系统内存是否持续增长 / OOM
#
# 用法:
#   ./scripts/monitor_mem.sh              # 每 2s 刷新，监控 vision_qt_demo
#   ./scripts/monitor_mem.sh vision_qt_demo 1
#   ./scripts/monitor_mem.sh 12345 5        # 指定 PID，5s 间隔
#
# 另开终端运行 ./run.sh，本脚本持续观察 RSS 是否单调上涨。

set -euo pipefail

TARGET="${1:-vision_qt_demo}"
INTERVAL="${2:-2}"

if ! [[ "$INTERVAL" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
  echo "interval must be a number (seconds)" >&2
  exit 1
fi

resolve_pid() {
  if [[ "$TARGET" =~ ^[0-9]+$ ]]; then
    if kill -0 "$TARGET" 2>/dev/null; then
      echo "$TARGET"
      return 0
    fi
    return 1
  fi
  pidof "$TARGET" 2>/dev/null | awk '{print $1}'
}

print_system_mem() {
  if command -v free >/dev/null 2>&1; then
    free -h | awk 'NR<=2 {print "  " $0}'
  else
    awk '/^MemTotal:|^MemAvailable:|^SwapTotal:|^SwapFree:/ {print "  " $0}' /proc/meminfo
  fi
}

print_proc_mem() {
  local pid="$1"
  if [ ! -r "/proc/$pid/status" ]; then
    echo "  (cannot read /proc/$pid/status)"
    return
  fi
  awk '
    /^Name:/     { name=$2 }
    /^VmRSS:/    { rss=$2 }
    /^VmSize:/   { vsz=$2 }
    /^VmSwap:/   { swap=$2 }
    /^Threads:/  { thr=$2 }
    END {
      printf "  process: %s (pid=%s)\n", name, pid
      printf "  VmRSS=%s kB (%.1f MiB)  VmSize=%s kB  VmSwap=%s kB  threads=%s\n",
        rss, rss/1024, vsz, swap, thr
    }
  ' pid="$pid" "/proc/$pid/status"
}

check_oom_recent() {
  if dmesg 2>/dev/null | tail -200 | grep -qiE 'out of memory|oom-killer|killed process'; then
    echo "  [WARN] dmesg 近期有 OOM / kill 记录，执行: dmesg | tail -50"
  fi
}

echo "========================================"
echo "[mem] target=$TARGET  interval=${INTERVAL}s"
echo "      Ctrl+C 停止"
echo "========================================"

LAST_RSS=""
GROW_STREAK=0

while true; do
  echo ""
  date '+%Y-%m-%d %H:%M:%S'
  print_system_mem
  check_oom_recent

  pid="$(resolve_pid || true)"
  if [ -z "$pid" ]; then
    echo "  $TARGET 未运行"
    LAST_RSS=""
    GROW_STREAK=0
  else
    print_proc_mem "$pid"
    cur_rss="$(awk '/^VmRSS:/ {print $2}' "/proc/$pid/status" 2>/dev/null || echo 0)"
    if [ -n "$LAST_RSS" ] && [ "$cur_rss" -gt "$LAST_RSS" ]; then
      GROW_STREAK=$((GROW_STREAK + 1))
      delta=$((cur_rss - LAST_RSS))
      echo "  RSS +${delta} kB (连续增长 ${GROW_STREAK} 次)"
      if [ "$GROW_STREAK" -ge 10 ]; then
        echo "  [WARN] RSS 连续 ${GROW_STREAK} 次上升，可能存在内存泄漏"
      fi
    else
      GROW_STREAK=0
    fi
    LAST_RSS="$cur_rss"
  fi

  sleep "$INTERVAL"
done
