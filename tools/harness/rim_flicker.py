#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rim_flicker.py - リムライトだけの揺らぎ (コマごとの震え) を、動きを差し引いて測る

  python tools/harness/rim_flicker.py --video a.mp4 --depth-dir cache/x/depth --start 30 --count 12 --extra "..." --var "rimsmooth=0" --var "rimsmooth=0.5"

リム = (リムあり − rim=0)。前のコマのリムをオプティカルフローで今のコマの位置へずらして比べる。
リムの帯は細いので、比べる前に少しだけ (1px) ぼかす。値は帯の上 (どちらかのコマでリムがある画素) の平均と 99%。
"""
import argparse
import sys
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from flicker_test import frames  # noqa: E402
from depth_ab import depth, neighbors  # noqa: E402
from flow_flicker import flow, valid_mask, warp  # noqa: E402
from run_cli import run  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", required=True)
    ap.add_argument("--depth-dir", required=True)
    ap.add_argument("--start", type=int, required=True)
    ap.add_argument("--count", type=int, default=12)
    ap.add_argument("--extra", default="")
    ap.add_argument("--var", action="append", required=True)
    a = ap.parse_args()
    srcs = frames(a.video, a.start, a.count)
    pairs = []
    for i in range(1, len(srcs)):
        f_cp = flow(srcs[i], srcs[i - 1])
        f_pc = flow(srcs[i - 1], srcs[i])
        pairs.append((f_cp, valid_mask(srcs[i], srcs[i - 1], f_cp, f_pc, warp(srcs[i - 1], f_cp))))
    base = a.extra.split() + ["gpu=1"]
    print(f"{'設定':<24} {'リムの量':>9} {'揺らぎ 平均':>11} {'揺らぎ 99%':>11}")
    for var in a.var:
        kv = var.split()
        rims = []
        for i, s in enumerate(srcs):
            n = a.start + i
            d, nb = depth(a.depth_dir, n), neighbors(a.depth_dir, n)
            on = run(s, d, base + kv, neighbors=nb)[:, :, :3]
            off = run(s, d, base + kv + ["rim=0"], neighbors=nb)[:, :, :3]
            rims.append(cv2.GaussianBlur(np.clip(on - off, 0, None).max(axis=2), (0, 0), 1.0))
        diffs, amount = [], []
        for i, (f, m) in enumerate(pairs, start=1):
            prev = warp(rims[i - 1], f)
            band = ((prev > 0.01) | (rims[i] > 0.01)) & m
            if band.any():
                diffs.append(np.abs(prev - rims[i])[band])
                amount.append(rims[i][band].mean())
        allv = np.concatenate(diffs)
        print(f"{var:<24} {np.mean(amount):9.4f} {allv.mean():11.4f} {np.percentile(allv, 99):11.4f}")


if __name__ == "__main__":
    main()
