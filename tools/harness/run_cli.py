#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
run_cli.py - relight_cli.exe (プラグインの照明コードそのもの) で 1 フレームを再照明し、比較画像を作る

  python tools/harness/run_cli.py --src image.png --depth cache/x/depth/depth.00001.png --out out.png -- lx=300 ly=400
  python tools/harness/run_cli.py --video a.mp4 --frame 10 --depth ... --out ...

出力は横並び: 元 / 深度 / 再照明 (Relit) / 影と凹み
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
CLI = HERE / "build" / "relight_cli.exe"


def load_png16(path):
    try:
        import cv2
        a = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
        if a.ndim == 3:
            a = a[:, :, ::-1]
        scale = 65535.0 if a.dtype == np.uint16 else 255.0
        return a.astype(np.float32) / scale
    except ImportError:
        return np.asarray(Image.open(path).convert("RGB")).astype(np.float32) / 255.0


def load_src(args):
    if args.video:
        tmp = Path(tempfile.gettempdir()) / "relight_cli_frame.png"
        subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", args.video, "-vf",
                        f"select=eq(n\\,{args.frame})", "-vframes", "1", str(tmp)], check=True)
        im = Image.open(tmp)
    else:
        im = Image.open(args.src)
    return np.asarray(im.convert("RGBA")).astype(np.float32) / 255.0


def run(src, depth, extra, mode=1, neighbors=None):
    """neighbors: [(時刻の差, 深度), ...] 前後のフレームの深度 (深度を時間方向にならす)"""
    H, W = src.shape[:2]
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        src.astype(np.float32).tofile(td / "src.f32")
        nb_args = []
        for k, (off, nd) in enumerate(neighbors or []):
            p = td / f"nb{k}.f32"
            nd.astype(np.float32).tofile(p)
            nb_args.append(f"nb={off}:{p}")
        if depth is not None:
            depth.astype(np.float32).tofile(td / "depth.f32")
        cmd = [str(CLI), str(td / "src.f32"), str(td / "depth.f32") if depth is not None else "-",
               str(td / "out.f32"), str(W), str(H), f"mode={mode}"] + list(extra) + nb_args
        if depth is not None:
            # 深度は推定したときの大きさのまま渡し、プラグインと同じ処理で引き伸ばす
            cmd += [f"dw={depth.shape[1]}", f"dh={depth.shape[0]}"]
        r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
        if r.returncode != 0:
            raise SystemExit(f"relight_cli failed: {r.stderr}")
        print(r.stderr.strip())
        return np.fromfile(td / "out.f32", dtype=np.float32).reshape(H, W, 4)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src")
    ap.add_argument("--video")
    ap.add_argument("--frame", type=int, default=0)
    ap.add_argument("--depth", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("extra", nargs="*", help="relight_cli に渡す key=value")
    a = ap.parse_args()

    src = load_src(a)
    H, W = src.shape[:2]
    d = load_png16(a.depth)
    if d.ndim == 3:
        d = d[:, :, 0]
    relit = run(src, d, a.extra, mode=1)
    shadow = run(src, d, a.extra, mode=4)
    shown = d
    if d.shape != (H, W):   # 並べて見せる用だけ同じ大きさにする
        shown = np.asarray(Image.fromarray((d * 65535).astype(np.uint16)).resize((W, H), Image.BILINEAR)).astype(np.float32) / 65535.0
    panels = [src[:, :, :3], np.repeat(shown[:, :, None], 3, axis=2), relit[:, :, :3], shadow[:, :, :3]]
    strip = np.concatenate([np.clip(p, 0, 1) for p in panels], axis=1)
    Image.fromarray((strip * 255 + 0.5).astype(np.uint8)).save(a.out)
    Image.fromarray((np.clip(relit[:, :, :3], 0, 1) * 255 + 0.5).astype(np.uint8)).save(
        str(Path(a.out).with_suffix("")) + "_relit.png")
    print(f"保存: {a.out}  (元 / 深度 / 再照明 / 影と凹み)")


if __name__ == "__main__":
    main()
