#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
depth_ab.py - 2 つの深度 (例: ならす前 / 後) で同じ照明を描いて、影の出方を比べる

  python tools/harness/depth_ab.py --video a.mp4 --a cache/x/depth --b cache/y/depth --frames 60,100,150 \
      --light "lx=900 ly=150 lz=1.2" [--light ...] [--out ab.png]

出す値 (人物の上だけでなく画面全体):
  暗く : 元の絵より暗くなった量の平均 (影の濃さ)
  影の面積 : 0.05 以上暗くなった画素の割合
  深度の勾配 : 深度の細かさの目安 (ならしで凹凸が均されていないか)
"""
import argparse
import sys
from pathlib import Path

import cv2
import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from flicker_test import frames  # noqa: E402
from run_cli import load_png16, run  # noqa: E402


def lum(img):
    return img[:, :, 0] * 0.299 + img[:, :, 1] * 0.587 + img[:, :, 2] * 0.114


def depth(dirpath, n):
    d = load_png16(Path(dirpath) / f"depth.{n + 1:05d}.png")
    return d[:, :, 0] if d.ndim == 3 else d


def neighbors(dirpath, n):
    out = []
    for off in (-1, 1, -2, 2):
        p = Path(dirpath) / f"depth.{n + off + 1:05d}.png"
        if p.exists():
            d = load_png16(p)
            out.append((off, d[:, :, 0] if d.ndim == 3 else d))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", required=True)
    ap.add_argument("--a", required=True)
    ap.add_argument("--b", required=True)
    ap.add_argument("--frames", default="60")
    ap.add_argument("--light", action="append", required=True)
    ap.add_argument("--out", default="")
    a = ap.parse_args()

    rows = []
    for n in [int(x) for x in a.frames.split(",")]:
        src = frames(a.video, n, 1)[0]
        da, db = depth(a.a, n), depth(a.b, n)
        ga = np.hypot(cv2.Sobel(da, cv2.CV_32F, 1, 0), cv2.Sobel(da, cv2.CV_32F, 0, 1)).mean()
        gb = np.hypot(cv2.Sobel(db, cv2.CV_32F, 1, 0), cv2.Sobel(db, cv2.CV_32F, 0, 1)).mean()
        print(f"フレーム {n}: 深度の差 平均 {np.abs(da - db).mean():.4f}  勾配 A {ga:.4f} / B {gb:.4f}")
        for light in a.light:
            res = []
            for dd, dirpath in ((da, a.a), (db, a.b)):
                out = run(src, dd, light.split() + ["gpu=1"], mode=1, neighbors=neighbors(dirpath, n))
                dark = np.clip(lum(src) - lum(out), 0, None)
                res.append((out, dark.mean(), (dark > 0.05).mean()))
            print(f"  [{light}] 暗く A {res[0][1]:.4f} / B {res[1][1]:.4f}   影の面積 A {res[0][2]:.3f} / B {res[1][2]:.3f}")
            if a.out:
                rows.append(np.concatenate([np.clip(r[0][:, :, :3], 0, 1) for r in res], axis=1))
    if a.out:
        img = np.concatenate(rows, axis=0)
        img = cv2.resize(img, (img.shape[1] // 2, img.shape[0] // 2), interpolation=cv2.INTER_AREA)
        Image.fromarray((img * 255).astype(np.uint8)).save(a.out)
        print(f"保存: {a.out} (左 A、右 B)")


if __name__ == "__main__":
    main()
