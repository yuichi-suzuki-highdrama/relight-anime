#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
flicker_map.py - 揺らぎがどこで起きているかを画像にする

  python tools/harness/flicker_map.py --video a.mp4 --depth-dir cache/x/depth --start 100 --count 8 --out map.png [--extra "..."]

元の絵が動いていない画素で、効果 (照明後 − 元の絵) のフレーム間の変化の平均を明るさにする (×20)。
左: 元の絵 (最初のフレーム) / 右: 揺らぎ
"""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from flicker_test import frames  # noqa: E402
from run_cli import load_png16, run  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", required=True)
    ap.add_argument("--depth-dir", required=True)
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--count", type=int, default=8)
    ap.add_argument("--extra", default="")
    ap.add_argument("--out", required=True)
    ap.add_argument("--gain", type=float, default=20.0)
    a = ap.parse_args()

    srcs = frames(a.video, a.start, a.count)

    def depth_at(n):
        p = Path(a.depth_dir) / f"depth.{n + 1:05d}.png"
        if n < 0 or not p.exists():
            return None
        d = load_png16(p)
        return d[:, :, 0] if d.ndim == 3 else d

    effects = []
    for i, s in enumerate(srcs):
        nb = [(o, depth_at(a.start + i + o)) for o in (-1, 1, -2, 2)]
        nb = [(o, d) for o, d in nb if d is not None]
        out = run(s, depth_at(a.start + i), a.extra.split() + ["gpu=1"], mode=1, neighbors=nb)[:, :, :3]
        effects.append(out - s[:, :, :3])
    acc = np.zeros(srcs[0].shape[:2], np.float32)
    cnt = np.zeros_like(acc)
    for i in range(len(srcs) - 1):
        still = np.abs(srcs[i + 1][:, :, :3] - srcs[i][:, :, :3]).max(axis=2) < 0.02
        ch = np.abs(effects[i + 1] - effects[i]).max(axis=2)
        acc += np.where(still, ch, 0)
        cnt += still
    m = np.clip(acc / np.maximum(cnt, 1) * a.gain, 0, 1)
    left = np.clip(srcs[0][:, :, :3], 0, 1)
    right = np.repeat(m[:, :, None], 3, axis=2)
    Image.fromarray((np.concatenate([left, right], axis=1) * 255).astype(np.uint8)).save(a.out)
    print(f"保存: {a.out}")


if __name__ == "__main__":
    main()
