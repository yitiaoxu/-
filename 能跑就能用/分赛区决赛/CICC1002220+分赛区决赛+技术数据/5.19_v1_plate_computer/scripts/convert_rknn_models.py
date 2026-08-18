# -*- coding: utf-8 -*-
"""
Linux PC/VM (RKNN Toolkit2): ONNX -> RKNN for plate detect & plate rec.

Requirements:
  - rknn-toolkit2 (e.g. 2.3.x) in conda/venv
  - ONNX + calibration images on the conversion machine

Examples (RK3568 INT8):
  python scripts/convert_rknn_models.py detect \\
    --onnx weights/plate_detect.onnx \\
    --out weights/plate_detect_int8.rknn \\
    --calib-root ./calibration_rk3568 \\
    --calib-list dataset_detect.txt

  python scripts/convert_rknn_models.py rec \\
    --onnx weights/plate_rec_color.onnx \\
    --out weights/plate_rec_color_int8.rknn \\
    --calib-root ./calibration_rk3568 \\
    --calib-list dataset_rec.txt

FP (no quant, same as plate_detect_fp.rknn):
  python scripts/convert_rknn_models.py detect \\
    --onnx weights/plate_detect.onnx \\
    --out weights/plate_detect_fp.rknn \\
    --no-quant
"""
from __future__ import annotations

import argparse
import os
import sys
import tempfile
from pathlib import Path


def import_rknn():
    try:
        from rknn.api import RKNN
        return RKNN
    except ImportError as exc:
        print("[ERROR] rknn.api.RKNN import failed. Install rknn-toolkit2:", exc, file=sys.stderr)
        sys.exit(1)


def make_absolute_dataset(calibration_root: Path, list_filename: str) -> tuple[str, int]:
    list_path = calibration_root / list_filename
    if not list_path.is_file():
        raise FileNotFoundError(list_path)

    paths: list[str] = []
    for raw in list_path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        img_path = (calibration_root / line).resolve()
        if not img_path.is_file():
            raise FileNotFoundError(f"Missing calibration image: {img_path}")
        paths.append(str(img_path))

    if not paths:
        raise ValueError(f"No entries in {list_path}")

    fd, tmp_path = tempfile.mkstemp(prefix="rknn_calib_", suffix=".txt", text=True)
    os.close(fd)
    Path(tmp_path).write_text("\n".join(paths) + "\n", encoding="utf-8")
    return tmp_path, len(paths)


def _parse_mv_sv(s: str) -> list[list[float]]:
    parts = [float(x) for x in s.replace(",", " ").split()]
    if len(parts) != 3:
        raise ValueError(f"Need three values for mean/std triple, got: {parts}")
    return [parts]


def run_convert(args: argparse.Namespace) -> int:
    RKNN = import_rknn()
    onnx_path = Path(args.onnx).resolve()
    out_path = Path(args.out).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    do_quant = not args.no_quant

    rknn = RKNN(verbose=args.verbose)

    rknn.config(
        mean_values=_parse_mv_sv(args.mean_values),
        std_values=_parse_mv_sv(args.std_values),
        target_platform=args.target_platform.strip(),
    )

    load_kwargs: dict = {"model": str(onnx_path)}
    inp = getattr(args, "inputs", "") or ""
    inp = inp.strip()
    if inp:
        load_kwargs["inputs"] = [x.strip() for x in inp.split(",")]

    isize = getattr(args, "input_sizes", None)
    isize = isize.strip() if isize else ""
    if isize:
        ints = [int(x) for x in isize.split()]
        load_kwargs["input_size_list"] = [ints]

    print("[INFO] load_onnx:", load_kwargs)
    ret = rknn.load_onnx(**load_kwargs)
    if ret != 0 and "input_size_list" in load_kwargs:
        load_kwargs.pop("input_size_list", None)
        print("[WARN] load_onnx failed; retry without input_size_list")
        print("[INFO] load_onnx:", load_kwargs)
        ret = rknn.load_onnx(**load_kwargs)

    if ret != 0:
        print("[ERROR] load_onnx ret=", ret)
        rknn.release()
        return ret

    tmp_ds: str | None = None
    if do_quant:
        calib_root = Path(args.calib_root).expanduser().resolve()
        tmp_ds, nimg = make_absolute_dataset(calib_root, args.calib_list)
        print("[INFO] INT8 calibration images:", nimg)
        ret = rknn.build(do_quantization=True, dataset=tmp_ds)
        try:
            os.unlink(tmp_ds)
        except OSError:
            pass
    else:
        ret = rknn.build(do_quantization=False)

    if ret != 0:
        print("[ERROR] build ret=", ret)
        rknn.release()
        return ret

    ret = rknn.export_rknn(str(out_path))
    if ret != 0:
        print("[ERROR] export_rknn ret=", ret)
    rknn.release()
    if ret == 0:
        print("[OK]", out_path)
    return ret


def main() -> None:
    parser = argparse.ArgumentParser(description="ONNX -> RKNN for plate pipeline")
    p = parser.add_subparsers(dest="cmd", required=True)

    d = p.add_parser("detect", help="plate_detect ONNX (1x3x640x640)")
    d.add_argument("--onnx", required=True)
    d.add_argument("--out", required=True)
    d.add_argument("--target-platform", default="rk3568")
    d.add_argument("--verbose", action="store_true")
    d.add_argument("--no-quant", action="store_true")
    d.add_argument("--calib-root", default="")
    d.add_argument("--calib-list", default="dataset_detect.txt")
    d.add_argument("--inputs", default="input")
    d.add_argument("--input-sizes", default="1 3 640 640")
    d.add_argument("--mean-values", dest="mean_values", default="0 0 0")
    d.add_argument("--std-values", dest="std_values", default="255 255 255")

    r = p.add_parser("rec", help="plate_rec_color ONNX (1x3x48x168)")
    r.add_argument("--onnx", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--target-platform", default="rk3568")
    r.add_argument("--verbose", action="store_true")
    r.add_argument("--no-quant", action="store_true")
    r.add_argument("--calib-root", default="")
    r.add_argument("--calib-list", default="dataset_rec.txt")
    r.add_argument("--inputs", default="input")
    r.add_argument("--input-sizes", default="1 3 48 168")
    r.add_argument("--mean-values", dest="mean_values", default="149.946 149.946 149.946")
    r.add_argument("--std-values", dest="std_values", default="49.215 49.215 49.215")

    args = parser.parse_args()

    do_quant = not args.no_quant
    if do_quant:
        if not getattr(args, "calib_root", "") or not str(args.calib_root).strip():
            print("[ERROR] INT8 requires --calib-root (folder with dataset txt and images)", file=sys.stderr)
            sys.exit(2)

    ec = run_convert(args)
    sys.exit(ec)


if __name__ == "__main__":
    main()
