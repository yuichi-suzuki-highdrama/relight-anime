#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
temporal_depth_test.py - 深度の時間方向のならし (素材の色を手がかりにする) を、できあがった深度 PNG に掛けて試す

  python tools/temporal_depth_test.py --video a.mp4 --depth-dir cache/x/depth --start 140 --count 30 --out-dir tmp/depth_t --radius 8

relight_depth.exe に入れる処理と同じ考え方:
  各画素で前後 radius フレームの深度を、時刻が近いほど、その画素の素材の色が変わっていないほど重く平均する。
  素材が動いていない所 (色が同じ) は深度も同じはずなので、深度推定のゆっくりした揺れが消える。
  動いている所は色が変わるので、ほとんど混ぜない (残像にならない)。
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image


def load_depth(p):
    a = np.asarray(Image.open(p)).astype(np.float32)
    if a.ndim == 3:
        a = a[:, :, 0]
    return a / 65535.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", required=True)
    ap.add_argument("--depth-dir", required=True)
    ap.add_argument("--start", type=int, required=True)
    ap.add_argument("--count", type=int, required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--radius", type=int, default=8)
    ap.add_argument("--sigma-c", type=float, default=0.05)
    ap.add_argument("--sigma-t", type=float, default=4.0)
    a = ap.parse_args()

    R = a.radius
    lo, hi = a.start - R, a.start + a.count - 1 + R
    names = sorted(Path(a.depth_dir).glob("depth.*.png"))
    total = len(names)
    lo, hi = max(0, lo), min(total - 1, hi)
    d0 = load_depth(names[0])
    dh, dw = d0.shape
    with tempfile.TemporaryDirectory() as td:
        subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", a.video, "-vf",
                        f"select=between(n\\,{lo}\\,{hi}),scale={dw}:{dh}:flags=area", "-vsync", "0",
                        str(Path(td) / "f%04d.png")], check=True)
        cols = {lo + i: np.asarray(Image.open(p).convert("RGB")).astype(np.float32) / 255.0
                for i, p in enumerate(sorted(Path(td).glob("f*.png")))}
    deps = {n: load_depth(names[n]) for n in range(lo, hi + 1)}
    out = Path(a.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    for n in range(lo, hi + 1):
        acc = np.zeros((dh, dw), np.float32)
        ws = np.zeros((dh, dw), np.float32)
        for k in range(-R, R + 1):
            m = n + k
            if m not in deps:
                continue
            diff = np.abs(cols[m] - cols[n]).mean(axis=2)
            w = np.exp(-0.5 * (k / a.sigma_t) ** 2) * np.exp(-(diff / a.sigma_c) ** 2)
            acc += deps[m] * w
            ws += w
        res = acc / ws
        Image.fromarray((np.clip(res, 0, 1) * 65535 + 0.5).astype(np.uint16)).save(out / names[n].name)
    print(f"保存: {out} ({hi - lo + 1} 枚)")


if __name__ == "__main__":
    main()
