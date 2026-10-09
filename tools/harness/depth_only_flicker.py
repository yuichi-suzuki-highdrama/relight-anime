#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
depth_only_flicker.py - 深度の揺れだけが原因の揺らぎを測る

  python tools/harness/depth_only_flicker.py --video a.mp4 --start 100 --count 16 --depth-dir A [--depth-dir B ...] [--extra "..."]

元の絵は start の 1 枚に固定し、深度だけを start から count 枚の実際の深度にして描く。
出てくる変化は深度の揺れによるものだけになる。ただし深度の中の「キャラの動き」も含むので、
深度の時間方向のならし方どうしを比べる (絶対値ではなく、どれだけ減ったか) のに使う。
広い範囲のゆっくりした揺れを見るため、ぼかした値 (sigma 8) も出す。
"""
import argparse
import sys
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from flicker_test import frames  # noqa: E402
from run_cli import load_png16, run  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", required=True)
    ap.add_argument("--depth-dir", action="append", required=True)
    ap.add_argument("--start", type=int, default=100)
    ap.add_argument("--count", type=int, default=16)
    ap.add_argument("--extra", default="")
    a = ap.parse_args()
    src = frames(a.video, a.start, 1)[0]
    print(f"{'深度':<60} {'揺らぎ':>9} {'ぼかし8':>9}")
    for dd in a.depth_dir:
        effects = []
        for i in range(a.count):
            p = Path(dd) / f"depth.{a.start + i + 1:05d}.png"
            d = load_png16(p)
            d = d[:, :, 0] if d.ndim == 3 else d
            out = run(src, d, a.extra.split() + ["gpu=1"], mode=1, neighbors=[])[:, :, :3]
            effects.append(out - src[:, :, :3])
        raw = np.mean([np.abs(effects[i + 1] - effects[i]).mean() for i in range(len(effects) - 1)])
        lo = np.mean([np.abs(cv2.GaussianBlur(effects[i + 1], (0, 0), 8) - cv2.GaussianBlur(effects[i], (0, 0), 8)).mean() for i in range(len(effects) - 1)])
        print(f"{dd[-60:]:<60} {raw:9.5f} {lo:9.5f}")


if __name__ == "__main__":
    main()
