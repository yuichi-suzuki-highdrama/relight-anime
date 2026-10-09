#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rim_view.py - リムライトだけを取り出して拡大して見る (ガタつきの確認)

  python tools/harness/rim_view.py --video a.mp4 --depth-dir cache/x/depth --frame 39 --extra "..." \
      --crop x,y,w,h [--zoom 3] --out rim.png [--frames 3]

リム = (リムあり − rim=0) の明るさ。--frames で続くコマも横に並べる (時間の揺れも見る)。
"""
import argparse
import sys
from pathlib import Path

import cv2
import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from flicker_test import frames  # noqa: E402
from depth_ab import depth, neighbors  # noqa: E402
from run_cli import run  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", required=True)
    ap.add_argument("--depth-dir", required=True)
    ap.add_argument("--frame", type=int, required=True)
    ap.add_argument("--frames", type=int, default=1)
    ap.add_argument("--extra", default="")
    ap.add_argument("--crop", default="")
    ap.add_argument("--zoom", type=int, default=3)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    base = a.extra.split() + ["gpu=1"]
    tiles = []
    for n in range(a.frame, a.frame + a.frames):
        src = frames(a.video, n, 1)[0]
        d = depth(a.depth_dir, n)
        nb = neighbors(a.depth_dir, n)
        on = run(src, d, base, neighbors=nb)[:, :, :3]
        off = run(src, d, base + ["rim=0"], neighbors=nb)[:, :, :3]
        rim = np.clip((on - off) * 2.0, 0, 1)
        vis = np.concatenate([np.clip(on, 0, 1), rim], axis=0)
        if a.crop:
            x, y, w, h = [int(v) for v in a.crop.split(",")]
            H = src.shape[0]
            vis = np.concatenate([np.clip(on, 0, 1)[y:y + h, x:x + w], rim[y:y + h, x:x + w]], axis=0)
            vis = cv2.resize(vis, (w * a.zoom, 2 * h * a.zoom), interpolation=cv2.INTER_NEAREST)
            _ = H
        tiles.append(vis)
    Image.fromarray((np.concatenate(tiles, axis=1) * 255).astype(np.uint8)).save(a.out)
    print(f"保存: {a.out}")


if __name__ == "__main__":
    main()
