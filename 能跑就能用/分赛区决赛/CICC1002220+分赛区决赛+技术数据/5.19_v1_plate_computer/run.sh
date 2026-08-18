#!/bin/bash
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

export LD_LIBRARY_PATH="${ROOT}/librknn_api/aarch64:${LD_LIBRARY_PATH}"

pick_tmpdir() {
  if [ -n "$TMPDIR" ] && mkdir -p "$TMPDIR" 2>/dev/null && [ -w "$TMPDIR" ]; then
    return 0
  fi
  for d in "$ROOT/tmp" "$HOME/tmp" "/tmp"; do
    if mkdir -p "$d" 2>/dev/null && [ -w "$d" ]; then
      export TMPDIR="$d"
      return 0
    fi
  done
  export TMPDIR="/tmp"
}
pick_tmpdir

# 有 HDMI/屏幕时不要 offscreen；仅无显示时用：
# export QT_QPA_PLATFORM=offscreen

load_pcie_driver() {
  local driver_dir="$1"
  echo "[INFO] PCIe driver: ${driver_dir}"
  cd "$driver_dir"
  if ! lsmod | grep -q pango_pci_driver; then
    make clean >/dev/null 2>&1 || true
    make 2>/dev/null || true
    if [ "$(id -u)" -eq 0 ]; then
      insmod pango_pci_driver.ko || true
    else
      sudo insmod pango_pci_driver.ko 2>/dev/null || insmod pango_pci_driver.ko 2>/dev/null || true
    fi
  fi
  cd "$ROOT"
}

fix_pcie_node_perm() {
  if [ ! -e /dev/pango_pci_driver ]; then
    echo "[WARN] /dev/pango_pci_driver 不存在，请先加载驱动"
    return 1
  fi
  if [ "$(id -u)" -eq 0 ]; then
    chmod 666 /dev/pango_pci_driver
  else
    sudo chmod 666 /dev/pango_pci_driver 2>/dev/null || chmod 666 /dev/pango_pci_driver 2>/dev/null || true
  fi
  if [ ! -r /dev/pango_pci_driver ] || [ ! -w /dev/pango_pci_driver ]; then
    echo "[WARN] 当前用户无读写权限，请执行:"
    echo "       sudo chmod 666 /dev/pango_pci_driver"
    return 1
  fi
  ls -la /dev/pango_pci_driver
  return 0
}

echo "==========================================="
echo "[INFO] 加载 PCIe 驱动（工程内 driver/）..."
PCIE_DRIVER="${ROOT}/driver"
if [ ! -d "$PCIE_DRIVER" ] || { [ ! -f "$PCIE_DRIVER/Makefile" ] && [ ! -f "$PCIE_DRIVER/pango_pci_driver.ko" ]; }; then
  echo "[WARN] ${PCIE_DRIVER} 未就绪，请执行: ./scripts/import_pcie_driver.sh"
else
  load_pcie_driver "$PCIE_DRIVER"
fi
fix_pcie_node_perm || true

echo "[INFO] 启动 vision_qt_demo ..."
echo "  目录: ${ROOT}"
echo "  模型: ${ROOT}/weights/"
echo "  驱动: ${ROOT}/driver/"
echo "  字体: ${ROOT}/fonts/platech.ttf"
echo "  PCIe: 对齐 QT上位机备份7.6（逐行 DMA + 单缓冲，无双缓冲）"
echo "==========================================="

# 强制与 7.6 一致：清掉会导致上下错位/撕裂的环境变量
unset PCIE_DOUBLE_BUF PCIE_USE_FRAME PCIE_FPGA_RGB888 PCIE_FPGA_ROI_PREVIEW PCIE_RGB565_SWAP PCIE_PREVIEW_ONLY PCIE_DBUF_KEEP_LATEST 2>/dev/null || true
export PCIE_DOUBLE_BUF=0
# 视频: 按片源帧率播放；默认每帧推理(VIDEO_SKIP_FRAMES=0)；仅当帧检测到才画框
#       省算力可设 VIDEO_SKIP_FRAMES=1 ；存盘: VIDEO_SAVE_FRAMES=1
export PLATE_PROFILE_EVERY="${PLATE_PROFILE_EVERY:-60}"
export PLATE_RESULT_EVERY="${PLATE_RESULT_EVERY:-60}"
export VIDEO_SKIP_FRAMES="${VIDEO_SKIP_FRAMES:-0}"
export VIDEO_REALTIME="${VIDEO_REALTIME:-1}"
unset VIDEO_BOX_HOLD VIDEO_VIZ_ALPHA 2>/dev/null || true

./build/vision_qt_demo

echo "[INFO] 已退出"
