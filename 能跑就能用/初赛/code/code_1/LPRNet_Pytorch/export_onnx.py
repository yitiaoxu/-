import torch

from data.load_data import CHARS
from model.LPRNet import build_lprnet


def main():
    pth_path = "./weights/best_ema.pth"
    onnx_path = "./weights/lprnet.onnx"

    device = torch.device("cpu")
    model = build_lprnet(
        lpr_max_len=8,
        phase=False,
        class_num=len(CHARS),
        dropout_rate=0,
    ).to(device)

    state = torch.load(pth_path, map_location=device)
    model.load_state_dict(state)
    model.eval()

    # LPRNet 输入：NCHW = [1, 3, 24, 94]
    dummy_input = torch.randn(1, 3, 24, 94, device=device)
    with torch.no_grad():
        out = model(dummy_input)
        print("output shape:", tuple(out.shape))  # 预期 (1, 68, 18)

    torch.onnx.export(
        model,
        dummy_input,
        onnx_path,
        input_names=["images"],
        output_names=["logits"],
        opset_version=11,
        do_constant_folding=True,
    )
    print("ONNX 导出完成:", onnx_path)


if __name__ == "__main__":
    main()

