#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

# 默认用工程内 tmp（linaro 对 /userdata/tmp 常无写权限）
pick_tmpdir() {
  if [ -n "$TMPDIR" ] && mkdir -p "$TMPDIR" 2>/dev/null && [ -w "$TMPDIR" ]; then
    return 0
  fi
  for d in "$ROOT/tmp" "$HOME/tmp" "/tmp"; do
    if mkdir -p "$d" 2>/dev/null && [ -w "$d" ]; then
      export TMPDIR="$d"
      echo "[INFO] TMPDIR=$TMPDIR"
      return 0
    fi
  done
  export TMPDIR="/tmp"
  echo "[WARN] fallback TMPDIR=/tmp"
}
pick_tmpdir

rm -rf ./build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2

echo "[OK] 可执行文件:"
echo "  ${ROOT}/build/vision_qt_demo"
echo "  ${ROOT}/build/test_rknn_infer/rknn_infer_one"
