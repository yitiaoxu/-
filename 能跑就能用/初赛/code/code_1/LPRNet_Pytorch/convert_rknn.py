"""
ONNX -> RKNN 转换脚本（RK3568）

用法：
1) 安装 rknn-toolkit2 的 Linux 环境运行（建议 Ubuntu x86 主机）
2) 准备好 ./weights/lprnet.onnx
3) 可选准备量化数据列表 ./dataset.txt（每行一张图片绝对路径）
4) 执行：python3 convert_rknn.py
"""

import os

from rknn.api import RKNN


def main():
    fixed_onnx_model = "./weights/lprnet_fixed.onnx"
    onnx_model = fixed_onnx_model if os.path.exists(fixed_onnx_model) else "./weights/lprnet.onnx"
    rknn_model = "./weights/lprnet_rk3568_new.rknn"
    dataset_txt = "./dataset.txt"

    if not os.path.exists(onnx_model):
        raise FileNotFoundError(f"未找到 ONNX 文件: {onnx_model}")

    rknn = RKNN(verbose=True)

    # 训练预处理：img = (img - 127.5) * 0.0078125 = (img - 127.5) / 128
    print("--> Config RKNN")
    ret = rknn.config(
        target_platform="rk3568",
        mean_values=[[127.5, 127.5, 127.5]],
        std_values=[[128.0, 128.0, 128.0]],
        # 你的 toolkit 版本不支持 reorder_channel，使用该开关控制 RGB/BGR 转换
        # 当前训练管线基于 OpenCV(BGR)，这里保持不转换即可
        quant_img_RGB2BGR=False,
    )
    if ret != 0:
        raise RuntimeError(f"rknn.config 失败: {ret}")

    print("--> Load ONNX")
    ret = rknn.load_onnx(model=onnx_model)
    if ret != 0:
        raise RuntimeError(f"rknn.load_onnx 失败: {ret}")

    do_quantization = os.path.exists(dataset_txt)
    print(f"--> Build RKNN (do_quantization={do_quantization})")
    if do_quantization:
        ret = rknn.build(do_quantization=True, dataset=dataset_txt)
    else:
        print("未找到 dataset.txt，改为不量化构建。")
        ret = rknn.build(do_quantization=False)
    if ret != 0:
        raise RuntimeError(f"rknn.build 失败: {ret}")

    print("--> Export RKNN")
    ret = rknn.export_rknn(rknn_model)
    if ret != 0:
        raise RuntimeError(f"rknn.export_rknn 失败: {ret}")

    rknn.release()
    print("RKNN 导出完成:", rknn_model)


if __name__ == "__main__":
    main()

