#!/bin/bash
# Build rknn_infer_one from parent project (shares plate_rknn_pipeline).
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
PARENT="$(cd "$ROOT/.." && pwd)"
cd "$PARENT"

pick_tmpdir() {
  if [ -n "$TMPDIR" ] && mkdir -p "$TMPDIR" 2>/dev/null && [ -w "$TMPDIR" ]; then
    return 0
  fi
  for d in "$PARENT/tmp" "$HOME/tmp" "/tmp"; do
    if mkdir -p "$d" 2>/dev/null && [ -w "$d" ]; then
      export TMPDIR="$d"
      echo "[INFO] TMPDIR=$TMPDIR"
      return 0
    fi
  done
  export TMPDIR="/tmp"
}
pick_tmpdir

rm -rf ./build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2 --target rknn_infer_one

echo "[OK] ${PARENT}/build/test_rknn_infer/rknn_infer_one"
