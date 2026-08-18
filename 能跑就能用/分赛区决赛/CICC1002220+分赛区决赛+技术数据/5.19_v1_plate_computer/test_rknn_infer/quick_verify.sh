#!/bin/bash
# 从 test123 随机抽 N 张，画框结果保存到 output/verify_时间戳/
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
PARENT="$(cd "$ROOT/.." && pwd)"
cd "$ROOT"

export LD_LIBRARY_PATH="${PARENT}/librknn_api/aarch64:${LD_LIBRARY_PATH}"

BIN="${PARENT}/build/test_rknn_infer/rknn_infer_one"
if [ ! -x "$BIN" ]; then
  BIN="${ROOT}/build/rknn_infer_one"
fi
if [ ! -x "$BIN" ]; then
  echo "[ERROR] 未找到 rknn_infer_one"
  echo "        请先执行: ./rebuild.sh  (在 test_rknn_infer 或项目根目录)"
  exit 1
fi

IMAGE_DIR="${VERIFY_DIR:-test123}"
LIMIT="${VERIFY_LIMIT:-10}"

ARGS=(--image-dir "$IMAGE_DIR" --limit "$LIMIT" --random --profile)
if [ "${VERIFY_NO_SAVE:-0}" = "1" ]; then
  ARGS+=(--no-save)
fi

echo "==========================================="
echo "[verify] dir:      $ROOT"
echo "[verify] models:   weights/ (INT8 > FP; PLATE_USE_FP=1 强制 FP)"
echo "[verify] images:   $IMAGE_DIR (random $LIMIT)"
echo "[verify] output:   output/verify_时间戳/"
echo "[verify] csv:      summary.csv (per-stage ms when --profile)"
echo "==========================================="

exec "$BIN" "${ARGS[@]}" "$@"
