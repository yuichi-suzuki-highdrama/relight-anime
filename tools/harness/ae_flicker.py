#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ae_flicker.py - AE で書き出した動画の「照明の効果」の揺らぎを測る (動きを差し引く)

  python tools/harness/ae_flicker.py --video 元.mp4 --start 60 --render out.mp4 [--render ...] [--strip strip.png --crop x,y,w,h]

効果 = 書き出し − 元の絵。元の絵からオプティカルフローを求め、前のコマの効果を今のコマの位置へずらして比べる。
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

import cv2
import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from flicker_test import frames  # noqa: E402
from flow_flicker import flow, valid_mask, warp  # noqa: E402


def read_all(video):
    with tempfile.TemporaryDirectory() as td:
        subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", video, str(Path(td) / "f%04d.png")], check=True)
        return [np.asarray(Image.open(p).convert("RGBA")).astype(np.float32) / 255.0 for p in sorted(Path(td).glob("f*.png"))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", required=True)
    ap.add_argument("--start", type=int, required=True)
    ap.add_argument("--render", action="append", required=True)
    ap.add_argument("--strip", default="")
    ap.add_argument("--crop", default="")
    a = ap.parse_args()
    outs = {r: read_all(r) for r in a.render}
    n = min(len(v) for v in outs.values())
    srcs = frames(a.video, a.start, n)
    pairs = []
    for i in range(1, n):
        f_cp = flow(srcs[i], srcs[i - 1])
        f_pc = flow(srcs[i - 1], srcs[i])
        pairs.append((f_cp, valid_mask(srcs[i], srcs[i - 1], f_cp, f_pc, warp(srcs[i - 1], f_cp))))
    print(f"{'書き出し':<40} {'揺らぎ':>9} {'ぼかし8':>9} {'最大 (ぼかし8)':>14}")
    strips = []
    for r, out in outs.items():
        eff = [out[i][:, :, :3] - srcs[i][:, :, :3] for i in range(n)]
        raw = np.mean([np.abs(warp(eff[i - 1], f) - eff[i]).max(axis=2)[m].mean() for i, (f, m) in enumerate(pairs, start=1)])
        lo = [np.abs(cv2.GaussianBlur(warp(eff[i - 1], f), (0, 0), 8) - cv2.GaussianBlur(eff[i], (0, 0), 8)).max(axis=2)[m] for i, (f, m) in enumerate(pairs, start=1)]
        print(f"{Path(r).name:<40} {raw:9.5f} {np.mean([x.mean() for x in lo]):9.5f} {np.max([np.percentile(x, 99) for x in lo]):14.4f}")
        if a.strip:
            row = []
            for i in range(min(n, 8)):
                vis = np.clip(0.5 + eff[i].mean(axis=2, keepdims=True) * 3, 0, 1).repeat(3, axis=2)
                if a.crop:
                    x, y, w, h = [int(v) for v in a.crop.split(",")]
                    vis = vis[y:y + h, x:x + w]
                row.append(vis)
            strips.append(np.concatenate(row, axis=1))
    if a.strip:
        Image.fromarray((np.concatenate(strips, axis=0) * 255).astype(np.uint8)).save(a.strip)
        print(f"保存: {a.strip}")


if __name__ == "__main__":
    main()
