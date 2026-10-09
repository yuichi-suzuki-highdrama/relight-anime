#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
flicker_test.py - 連続するフレームを描き、効果の「揺らぎ」を測る

  python tools/harness/flicker_test.py --video a.mp4 --depth-dir cache/x/depth --start 100 --count 8 [--extra "preset=2 ..."]

揺らぎ = 効果 (照明後 − 元の絵) のフレーム間の変化の平均。元の絵の動きそのものは差し引かれる。
要素を 1 つずつ外したときの値と比べ、どこが揺らぎを生んでいるかを見る。
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_cli import load_png16, run  # noqa: E402


def frames(video, start, count):
    with tempfile.TemporaryDirectory() as td:
        subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", video, "-vf",
                        f"select=between(n\\,{start}\\,{start + count - 1})", "-vsync", "0",
                        str(Path(td) / "f%03d.png")], check=True)
        return [np.asarray(Image.open(p).convert("RGBA")).astype(np.float32) / 255.0
                for p in sorted(Path(td).glob("f*.png"))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", required=True)
    ap.add_argument("--depth-dir", required=True)
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--count", type=int, default=8)
    ap.add_argument("--extra", default="")
    ap.add_argument("--parts", action="store_true", help="要素を 1 つずつ外して測る")
    ap.add_argument("--fixed-depth", action="store_true", help="全フレームに最初のフレームの深度を使う (揺らぎが深度から来ているかを見る)")
    a = ap.parse_args()

    srcs = frames(a.video, a.start, a.count)

    def depth_at(n):
        p = Path(a.depth_dir) / f"depth.{n + 1:05d}.png"
        if n < 0 or not p.exists():
            return None
        d = load_png16(p)
        return d[:, :, 0] if d.ndim == 3 else d

    depths = [depth_at(a.start + i) for i in range(len(srcs))]
    if a.fixed_depth:
        depths = [depths[0]] * len(depths)

    def neighbors(i, radius):
        out = []
        for off in [-1, 1, -2, 2][: radius * 2]:
            nd = depth_at(a.start + i + off)
            if nd is not None:
                out.append((off, nd))
        return out
    base = a.extra.split() + ["gpu=1"]
    # (名前, key=value, 深度を時間方向にならす前後のフレーム数)
    sets = [
        ("以前の作り (立体的・ならしなし)", ["form=1"], 0),
        ("今の既定", [], 2),
    ]
    if a.parts:
        sets += [
            ("今の既定 - リム", ["rim=0"], 2),
            ("今の既定 - 影色", ["tint=0"], 2),
            ("今の既定 - 線画の保護", ["line=0"], 2),
            ("今の既定 - 線画合わせ", ["snap=0"], 2),
            ("今の既定 - ディザ", ["dither=0"], 2),
            ("今の既定 - 膨らみ", ["volume=0"], 2),
            ("今の既定 - セル調", ["cel=0"], 2),
            ("今の既定 - 光 (影色・リムだけ)", ["intensity=0.01"], 2),
            ("今の既定 - 落ちる影", ["shadow=0"], 2),
            ("今の既定 - すき間の暗がり", ["occlusion=0"], 2),
            ("今の既定 - 立体の混ぜ (form=0)", ["form=0"], 2),
            ("今の既定 - 落ちる影・暗がり・立体", ["shadow=0", "occlusion=0", "form=0"], 2),
        ]
    dep = np.mean([np.abs(depths[i + 1] - depths[i]).mean() for i in range(len(depths) - 1)])
    print(f"深度そのもののフレーム間の変化: {dep:.5f}")
    # 元の絵が動いていない画素 (前後のフレームでほぼ同じ色)。ここでの効果の変化は純粋な揺らぎ
    still = [np.abs(srcs[i + 1][:, :, :3] - srcs[i][:, :, :3]).max(axis=2) < 0.02 for i in range(len(srcs) - 1)]
    print(f"動いていない画素の割合: {np.mean([s.mean() for s in still]):.2f}")
    print(f"{'設定':<30} {'揺らぎ':>10} {'静止部分の揺らぎ':>14}")
    for name, kv, radius in sets:
        effects = []
        for i, (s, d) in enumerate(zip(srcs, depths)):
            nb = [] if a.fixed_depth else neighbors(i, radius)
            out = run(s, d, base + kv, mode=1, neighbors=nb)[:, :, :3]
            effects.append(out - s[:, :, :3])
        flick = np.mean([np.abs(effects[i + 1] - effects[i]).mean() for i in range(len(effects) - 1)])
        still_f = np.mean([np.abs(effects[i + 1] - effects[i]).max(axis=2)[still[i]].mean() for i in range(len(effects) - 1)])
        print(f"{name:<30} {flick:10.5f} {still_f:14.5f}")


if __name__ == "__main__":
    main()
