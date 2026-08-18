#!/bin/sh

echo "==========================================="
echo "[INFO] 正在加载 PCIe 驱动模块..."

cd /userdata/02_pcie_image_test_720p/driver
insmod pango_pci_driver.ko 2>/dev/null
echo "[WARNING] 驱动已存在或加载失败"

echo "[INFO] 配置设备节点权限..."
chmod 666 /dev/pango_pci_driver 2>/dev/null
chmod 666 /dev/xdma0_c2h_0 2>/dev/null

echo "==========================================="
echo "[INFO] 正在启动 Vision Qt Demo 界面..."

# ? 关键：回到 code 目录
cd /userdata/code
export QT_QPA_PLATFORM=offscreen

./build/vision_qt_demo

echo "==========================================="
echo "[INFO] Qt 界面已关闭，正在清理 PCIe 驱动..."

cd /userdata/02_pcie_image_test_720p/driver
rmmod pango_pci_driver.ko 2>/dev/null
echo "[SUCCESS] 驱动已卸载，运行结束！"