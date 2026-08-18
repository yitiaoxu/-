# RK3568 车牌识别 — 5.19_v1_computer

板端路径：`/userdata/CY777/5.19_v1_plate_computer`（所有资源应在此目录内）

Qt 上位机 `vision_qt_demo` 与 CLI `rknn_infer_one` 共用 **`src/plate/plate_rknn_pipeline.cpp`**（`plate_detect_fp` + `plate_rec_fp`）。

## 目录（自包含）

```
5.19_v1_plate_computer/
  rebuild.sh / run.sh
  scripts/import_pcie_driver.sh   # 一次性导入 PCIe 驱动
  CMakeLists.txt
  librknn_api/                    # RKNN 运行时
  driver/                         # PCIe 1080p 驱动（pango_pci_driver）
  fonts/platech.ttf               # 中文叠加字体
  weights/                        # RKNN 模型（推荐放这里）
    plate_detect_fp.rknn
    plate_rec_fp.rknn
  src/plate/                      # 核心算法
  src/qt0323/                     # Qt 界面 + PCIe 采集
  test_rknn_infer/                # CLI + 测试图 test123/
  output/                         # 运行输出（batch_时间戳/）
  build/                          # 编译产物
  tmp/
```

## 首次部署（板端）

```bash
cd /userdata/CY777/5.19_v1_plate_computer
chmod +x rebuild.sh run.sh scripts/import_pcie_driver.sh

# 1. 导入 PCIe 1080p 驱动到本工程 driver/
./scripts/import_pcie_driver.sh

# 2. 确认模型与字体
ls weights/plate_detect_fp.rknn weights/plate_rec_fp.rknn
ls fonts/platech.ttf    # 若无则从 test_rknn_infer/fonts/ 复制

# 3. 编译
./rebuild.sh
```

## 依赖（首次）

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config \
  qtbase5-dev qt5-qmake libopencv-dev
# 中文标签需要 OpenCV freetype 模块（板端包名因系统而异）
```

## 编译

```bash
cd /userdata/CY777/5.19_v1_plate_computer
chmod +x rebuild.sh run.sh
./rebuild.sh
# 临时目录默认使用工程内 ./tmp（无需 sudo）
```

产物：

| 程序 | 路径 |
|------|------|
| Qt 上位机 | `build/vision_qt_demo` |
| CLI | `build/test_rknn_infer/rknn_infer_one` |

## 运行 Qt

```bash
cd /userdata/5.19_v1_computer
export LD_LIBRARY_PATH=/userdata/5.19_v1_computer/librknn_api/aarch64:$LD_LIBRARY_PATH
./run.sh
# 或
./build/vision_qt_demo
```

界面：纯白极简工控风（左侧导航、顶栏状态、主可视化区、右下状态栏）。功能：**批量图片文件夹**、**PCIe 实时采集**；批量推理带进度条与结果翻页。

## 快速验证新 RKNN 模型（推荐）

替换 `weights/*.rknn` 后：

```bash
cd /userdata/CY777/5.19_v1_plate_computer
./rebuild.sh   # 源码有改动时
chmod +x test_rknn_infer/quick_verify.sh
./test_rknn_infer/quick_verify.sh              # 冒烟 10 张，不存图
VERIFY_LIMIT=0 VERIFY_SAVE=1 ./test_rknn_infer/quick_verify.sh   # 全量 + CSV
PLATE_USE_FP=1 ./test_rknn_infer/quick_verify.sh                 # 强制 FP
```

详见 `test_rknn_infer/README.md`。

## 运行 CLI（批量）

```bash
cd /userdata/CY777/5.19_v1_plate_computer
export LD_LIBRARY_PATH=$PWD/librknn_api/aarch64:$LD_LIBRARY_PATH

./build/test_rknn_infer/rknn_infer_one --image-dir test_rknn_infer/test123
```

模型自动从 `weights/` 加载（INT8 优先）；批量结果保存到 `output/verify_YYYYMMDD_HHMMSS/` + `summary.csv`。

FP 检测模型请固定 **`--input-mode uint8_nhwc`**，避免 `auto` 时刷 `E RKNN` 错误。

## PCIe（1080p，工程内 driver/）

`run.sh` 自动从 **`./driver/`** 加载 `pango_pci_driver.ko`。

与 `code_4_v2` 一致：1920×1080 RGB565、链路检测、首帧 dump 到 `output/pcie_frame.bin`。

```bash
cd /userdata/CY777/5.19_v1_plate_computer
./run.sh
```

手动加载（调试）：

```bash
sudo insmod ./driver/pango_pci_driver.ko
sudo chmod 666 /dev/pango_pci_driver
```

## 常见问题

- **CMake 报 `plate_rknn_pipeline` 重复**：请用本目录最新 `test_rknn_infer/CMakeLists.txt`（子目录不再重复 `add_library`）。
- **根分区满 / 无权限建 /userdata/tmp**：脚本会自动用 `./tmp` 或 `$HOME/tmp`。
- **启动找不到模型**：把 `.rknn` 放到 `test_rknn_infer/` 或 `weights/`。
- **中文标签只有字母**：编译日志需有 `Chinese labels: ENABLED`；并放置 `fonts/platech.ttf`。
