#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
depth_test.py - relight_depth.exe を動画で試し、元の PyTorch 実装 (infer_video_depth) と比べる

  python tools/depth_test.py --video <mp4> [--frames 192] [--provider cuda|dml|cpu] [--no-ref]
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "depth" / "build" / "relight_depth.exe"
ENCODER = ROOT / "models" / "vda_small_encoder.onnx"
HEAD = ROOT / "models" / "vda_small_head_t32.onnx"
OUTDIR = ROOT / "cache" / "depth_test"

ORT_CUDA = Path(r"C:\ComfyUI_windows_portable\python_embeded\Lib\site-packages\onnxruntime\capi\onnxruntime.dll")
ORT_DML = Path(r"C:\Program Files\Adobe\Adobe After Effects 2026\Support Files\onnxruntime.dll")
def _torch_lib():
    """cuDNN の DLL を借りる torch の lib フォルダ (この Python に torch が入っていれば)"""
    import importlib.util
    spec = importlib.util.find_spec("torch")
    return [Path(spec.origin).parent / "lib"] if spec and spec.origin else []


DLL_DIRS = [Path(os.environ.get("CUDA_PATH", r"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8")) / "bin"] + _torch_lib()


def probe(video):
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                          "stream=width,height,r_frame_rate", "-of", "json", video], capture_output=True, text=True).stdout
    s = json.loads(out)["streams"][0]
    num, den = s["r_frame_rate"].split("/")
    return int(s["width"]), int(s["height"]), float(num) / float(den)


def decode(video, w, h, limit):
    cmd = ["ffmpeg", "-v", "error", "-i", video, "-f", "rawvideo", "-pix_fmt", "rgb24"]
    if limit:
        cmd += ["-frames:v", str(limit)]
    raw = subprocess.run(cmd + ["-"], capture_output=True, check=True).stdout
    n = len(raw) // (w * h * 3)
    return np.frombuffer(raw, np.uint8)[: n * w * h * 3].reshape(n, h, w, 3)


def write_frames(path, frames, fps):
    n, h, w, _ = frames.shape
    with open(path, "wb") as f:
        f.write(b"RLFR" + struct.pack("<IIIIIf", 1, w, h, n, 3, fps))
        f.write(frames.tobytes())


def read_depth(path):
    with open(path, "rb") as f:
        hdr = f.read(4 + 4 * 4 + 4 * 3)
        magic = hdr[:4]
        ver, w, h, n = struct.unpack("<IIII", hdr[4:20])
        lo, hi, fps = struct.unpack("<fff", hdr[20:32])
        assert magic == b"RLDP", magic
        d = np.frombuffer(f.read(), np.uint16).reshape(n, h, w).astype(np.float32) / 65535.0
    return d


def reference(frames):
    """元の実装 (fp32) で同じフレームを処理し、2%〜98% で正規化"""
    import torch
    sys.path.insert(0, str(ROOT / "tools"))
    from export_vda_onnx import load_model
    model = load_model(for_export=False).cuda()
    t0 = time.time()
    with torch.no_grad():
        res = model.infer_video_depth(frames, input_size=518, device="cuda", fp32=True,
                                      pbar=type("P", (), {"update": lambda self, n: None})())
    depths = res[0] if isinstance(res, tuple) else res
    dt = time.time() - t0
    del model
    torch.cuda.empty_cache()
    d = np.stack(depths) if isinstance(depths, list) else np.asarray(depths)
    lo, hi = np.percentile(d[:, ::4, ::4], [2, 98])
    return np.clip((d - lo) / (hi - lo), 0, 1), dt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", required=True)
    ap.add_argument("--frames", type=int, default=0)
    ap.add_argument("--provider", default="cuda")
    ap.add_argument("--no-ref", action="store_true")
    ap.add_argument("--profile", action="store_true", help="ONNX Runtime のプロファイルを取り、処理ごとの時間を集計")
    ap.add_argument("--cuda-opt", action="append", default=[], help="CUDA の設定 key=value")
    ap.add_argument("--chunk", type=int, default=4, help="エンコーダに一度に通すフレーム数")
    a = ap.parse_args()

    OUTDIR.mkdir(parents=True, exist_ok=True)
    w, h, fps = probe(a.video)
    frames = decode(a.video, w, h, a.frames)
    fpath, dpath, spath = OUTDIR / "frames.bin", OUTDIR / f"depth_{a.provider}.bin", OUTDIR / "status.json"
    write_frames(fpath, frames, fps)
    print(f"フレーム {frames.shape[0]} 枚 {w}x{h} @ {fps:.2f}fps")

    ort = ORT_DML if a.provider == "dml" else ORT_CUDA
    cmd = [str(EXE), "--frames", str(fpath), "--out", str(dpath), "--encoder", str(ENCODER), "--head", str(HEAD),
           "--provider", a.provider, "--ort", str(ort), "--status", str(spath)]
    for d in DLL_DIRS:
        cmd += ["--dll-dir", str(d)]
    if a.profile:
        cmd += ["--profile", str(OUTDIR / "ort_profile")]
    cmd += ["--chunk", str(a.chunk)]
    for kv in a.cuda_opt:
        cmd += ["--cuda-opt", kv]
    t0 = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    print(r.stdout.strip())
    warn = [l for l in r.stderr.splitlines() if l.strip()]
    if warn:
        print("--- stderr (先頭 15 行) ---")
        print("\n".join(warn[:15]))
    if r.returncode != 0:
        raise SystemExit(f"relight_depth が失敗 (code {r.returncode})")
    print(f"relight_depth 合計 {time.time() - t0:.1f}s (プロセス起動込み)")

    if a.profile:
        prof = sorted(OUTDIR.glob("ort_profile*.json"), key=lambda p: p.stat().st_mtime)[-1]
        ev = json.load(open(prof, encoding="utf-8"))
        agg = {}
        for e in ev:
            if e.get("cat") == "Node" and e.get("name", "").endswith("_kernel_time"):
                key = (e["args"].get("op_name"), e["args"].get("provider"))
                agg[key] = agg.get(key, 0) + e["dur"]
        tot = sum(agg.values())
        print(f"--- 処理ごとの合計時間 (全 {tot / 1e6:.2f}s) ---")
        for (op, prov), us in sorted(agg.items(), key=lambda kv: -kv[1])[:15]:
            print(f"  {op:22s} {prov:24s} {us / 1e6:7.2f}s {100 * us / tot:5.1f}%")
    ours = read_depth(dpath)
    if a.no_ref:
        return
    ref, dt = reference(frames)
    print(f"元の実装 (PyTorch fp32, CUDA): {dt:.1f}s")
    # 比較は元の実装の解像度 (フレーム解像度) に合わせる
    import cv2
    ours_up = np.stack([cv2.resize(o, (w, h), interpolation=cv2.INTER_LINEAR) for o in ours])
    diff = np.abs(ours_up - ref)
    corr = np.corrcoef(ours_up[:, ::4, ::4].ravel(), ref[:, ::4, ::4].ravel())[0, 1]
    per_frame = diff.reshape(diff.shape[0], -1).mean(1)
    print(f"一致度: 相関 {corr:.5f}, 平均絶対差 {diff.mean():.4f} (0..1), 最悪フレームの平均差 {per_frame.max():.4f} (#{per_frame.argmax()})")
    # 比較画像: 3 フレーム分 (元 / 元の実装 / relight_depth)
    from PIL import Image
    rows = []
    for i in [0, len(ref) // 2, len(ref) - 1]:
        rows.append(np.concatenate([frames[i] / 255.0, np.repeat(ref[i][..., None], 3, 2), np.repeat(ours_up[i][..., None], 3, 2)], 1))
    Image.fromarray((np.concatenate(rows, 0) * 255).astype(np.uint8)).resize(
        (rows[0].shape[1] // 2, len(rows) * rows[0].shape[0] // 2)).save(OUTDIR / f"compare_{a.provider}.png")
    print(f"比較画像: {OUTDIR / f'compare_{a.provider}.png'} (元 / 元の実装 / relight_depth)")


if __name__ == "__main__":
    main()
