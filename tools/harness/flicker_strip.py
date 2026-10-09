#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
flicker_strip.py - 連続するコマの「照明の効果」を並べた画像を作る (揺らぎを目で見るため)

  python tools/harness/flicker_strip.py --video a.mp4 --depth-dir D --start 100 --count 8 --out strip.png [--extra "..."] [--crop x,y,w,h]

上の段: 照明後の絵。下の段: 照明の効果 (照明後 − 元の絵) を強調したもの (灰色 = 変化なし、明るい = 光、暗い = 影)。
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
    ap.add_argument("--start", type=int, default=100)
    ap.add_argument("--count", type=int, default=8)
    ap.add_argument("--out", required=True)
    ap.add_argument("--extra", default="")
    ap.add_argument("--crop", default="", help="x,y,w,h")
    ap.add_argument("--gain", type=float, default=3.0)
    ap.add_argument("--radius", type=int, default=2)
    a = ap.parse_args()
    srcs = frames(a.video, a.start, a.count)

    def depth_at(n):
        p = Path(a.depth_dir) / f"depth.{n + 1:05d}.png"
        if n < 0 or not p.exists():
            return None
        d = load_png16(p)
        return d[:, :, 0] if d.ndim == 3 else d

    tops, bots = [], []
    for i, s in enumerate(srcs):
        n = a.start + i
        nb = []
        for off in [-1, 1, -2, 2][: a.radius * 2]:
            nd = depth_at(n + off)
            if nd is not None:
                nb.append((off, nd))
        out = run(s, depth_at(n), a.extra.split() + ["gpu=1"], mode=1, neighbors=nb)
        eff = out[:, :, :3] - s[:, :, :3]
        vis = np.clip(0.5 + eff.mean(axis=2, keepdims=True) * a.gain, 0, 1).repeat(3, axis=2)
        top = np.clip(out[:, :, :3], 0, 1)
        if a.crop:
            x, y, w, h = [int(v) for v in a.crop.split(",")]
            top, vis = top[y:y + h, x:x + w], vis[y:y + h, x:x + w]
        tops.append(top)
        bots.append(vis)
    sheet = np.concatenate([np.concatenate(tops, axis=1), np.concatenate(bots, axis=1)], axis=0)
    Image.fromarray((sheet * 255).astype(np.uint8)).save(a.out)
    print(f"保存: {a.out} {sheet.shape[1]}x{sheet.shape[0]}")


if __name__ == "__main__":
    main()
