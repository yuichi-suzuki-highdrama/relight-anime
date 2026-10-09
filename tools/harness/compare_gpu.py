#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
compare_gpu.py - 同じフレームを CPU 版と GPU 版で描き、差と時間を比べる

  python tools/harness/compare_gpu.py --video a.mp4 --frame 10 --depth cache/x/depth/depth.00011.png [--out diff.png]

プリセット全部と、出力の種類 (Depth / Normals / Shadow / Line Mask) で比べる。
差は 0..1 の表示の値で、最大と平均。8bit で 1 段 = 0.0039。
"""
import argparse
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_cli import load_src, load_png16, run  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src")
    ap.add_argument("--video")
    ap.add_argument("--frame", type=int, default=0)
    ap.add_argument("--depth", required=True)
    ap.add_argument("--out", help="一番差の大きい設定の差を画像にする")
    ap.add_argument("--extra", default="", help="全部に付ける key=value (空白区切り)")
    a = ap.parse_args()

    src = load_src(a)
    d = load_png16(a.depth)
    if d.ndim == 3:
        d = d[:, :, 0]
    extra = a.extra.split()

    cases = [(f"preset {i}", [f"preset={i}"], 1) for i in range(6)]
    cases += [("Depth", ["preset=0"], 2), ("Normals", ["preset=0"], 3), ("Shadow", ["preset=0"], 4), ("Line Mask", ["preset=0"], 5)]
    worst = None
    print(f"{'設定':<12} {'最大の差':>10} {'平均の差':>10}   CPU / GPU の時間")
    for name, kv, mode in cases:
        cpu = run(src, d, kv + extra, mode=mode)[:, :, :3]
        gpu = run(src, d, kv + extra + ["gpu=1"], mode=mode)[:, :, :3]
        diff = np.abs(np.clip(cpu, 0, 1) - np.clip(gpu, 0, 1))
        print(f"{name:<12} {diff.max():10.5f} {diff.mean():10.6f}")
        if worst is None or diff.max() > worst[0]:
            worst = (diff.max(), name, diff)
    if a.out and worst is not None:
        img = np.clip(worst[2].max(axis=2) * 50.0, 0, 1)   # 差を 50 倍で見せる
        Image.fromarray((img * 255).astype(np.uint8)).save(a.out)
        print(f"最も差が大きいのは {worst[1]} (差 ×50 の画像: {a.out})")


if __name__ == "__main__":
    main()
