#!/bin/bash
# 一次性：把板端已有的 1080p PCIe 驱动导入本工程 driver/
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${ROOT}/driver"
mkdir -p "$DEST"

SOURCES=(
  "/userdata/work/code_5/02_pcie_image_test_1080p/driver"
  "/userdata/CY777/02_pcie_image_test_1080p/driver"
  "/userdata/02_pcie_image_test_1080p/driver"
)

SRC=""
for d in "${SOURCES[@]}"; do
  if [ -f "${d}/Makefile" ] || [ -f "${d}/pango_pci_driver.ko" ]; then
    SRC="$d"
    break
  fi
done

if [ -z "$SRC" ]; then
  echo "[ERROR] 未找到 1080p 驱动目录，请手动拷贝到: ${DEST}"
  echo "  例如: cp -a /path/to/02_pcie_image_test_1080p/driver/* ${DEST}/"
  exit 1
fi

echo "[INFO] 从 ${SRC} 导入 -> ${DEST}"
cp -a "${SRC}/." "${DEST}/"
echo "[OK] driver/ 内容:"
ls -la "${DEST}"

if [ ! -f "${DEST}/pango_pci_driver.ko" ] && [ -f "${DEST}/Makefile" ]; then
  echo "[INFO] 未找到 .ko，尝试在 driver/ 内 make ..."
  (cd "${DEST}" && make clean >/dev/null 2>&1 || true && make)
fi

if [ -f "${DEST}/pango_pci_driver.ko" ]; then
  echo "[OK] pango_pci_driver.ko 已就绪"
else
  echo "[WARN] 仍无 pango_pci_driver.ko，请在 driver/ 下手动 make"
fi
