#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
flow_depth_stabilize.py - 深度を「動きに沿って」時間方向にならす (試作)

  python tools/flow_depth_stabilize.py --video a.mp4 --depth-dir cache/x/depth --out-dir tmp/depth_flow [--start 0 --count 0] [--keep 0.8]

深度の推定は、キャラの動きとは関係なくゆっくり揺れる (Video Depth Anything でも残る)。
その揺れを照明が明るさの揺れに変えて、ろうそくの炎のように見える。
同じ画素どうしで平均すると動いている所がぼける (残像) ので、オプティカルフロー (DIS) で
前のコマの「ならした深度」を今のコマの位置へずらしてから混ぜる (前向き)。後ろ向きにも同じことをして平均する。
フローが信頼できない所 (行って戻ると位置がずれる・ずらした色が合わない = 隠れた/現れた所) は混ぜない。

keep: 前のコマのならした深度をどれだけ残すか (0.8 なら、だいたい 5 コマ分の平均)
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

import cv2
import numpy as np


def read_frames(video, lo, hi, size):
    with tempfile.TemporaryDirectory() as td:
        subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", video, "-vf",
                        f"select=between(n\\,{lo}\\,{hi}),scale={size[0]}:{size[1]}:flags=area", "-vsync", "0",
                        str(Path(td) / "f%05d.png")], check=True)
        return [cv2.imread(str(p), cv2.IMREAD_COLOR) for p in sorted(Path(td).glob("f*.png"))]


def flow(a, b):
    """a の各画素が b のどこへ動いたか"""
    dis = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
    return dis.calc(cv2.cvtColor(a, cv2.COLOR_BGR2GRAY), cv2.cvtColor(b, cv2.COLOR_BGR2GRAY), None)


def warp(img, fl):
    h, w = fl.shape[:2]
    gx, gy = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
    return cv2.remap(img, gx + fl[:, :, 0], gy + fl[:, :, 1], cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)


def confidence(cur, other, f_co, f_oc):
    """cur の画素を other へ対応させたときの信頼度 (0..1)。行って戻るずれと、ずらした色の差で決める"""
    back = warp(f_oc, f_co)
    fb = np.linalg.norm(f_co + back, axis=2)
    col = np.abs(warp(other.astype(np.float32), f_co) - cur.astype(np.float32)).max(axis=2) / 255.0
    c = np.clip(1.0 - fb / 1.5, 0, 1) * np.clip(1.0 - col / 0.08, 0, 1)
    return cv2.GaussianBlur(c.astype(np.float32), (0, 0), 1.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", required=True)
    ap.add_argument("--depth-dir", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--count", type=int, default=0, help="0 なら最後まで")
    ap.add_argument("--keep", type=float, default=0.8)
    ap.add_argument("--mode", default="flow", help="flow: フローで位置を合わせる / color: 同じ画素で、色が変わっていない所だけ混ぜる")
    a = ap.parse_args()

    names = sorted(Path(a.depth_dir).glob("depth.*.png"))
    lo = a.start
    hi = len(names) - 1 if a.count <= 0 else min(len(names) - 1, a.start + a.count - 1)
    raw = [cv2.imread(str(names[n]), cv2.IMREAD_UNCHANGED) for n in range(lo, hi + 1)]
    dtype = raw[0].dtype
    scale = 65535.0 if dtype == np.uint16 else 255.0
    depth = [(r[:, :, 0] if r.ndim == 3 else r).astype(np.float32) / scale for r in raw]
    h, w = depth[0].shape
    src = read_frames(a.video, lo, hi, (w, h))
    n = min(len(src), len(depth))
    print(f"{n} フレーム ({w}x{h})")

    if a.mode == "color":
        zero = np.zeros((h, w, 2), np.float32)
        f_next = [zero] * (n - 1)
        f_prev = [zero] * (n - 1)
        def ccol(x, y):
            d = np.abs(x.astype(np.float32) - y.astype(np.float32)).max(axis=2) / 255.0
            return cv2.GaussianBlur(np.clip(1.0 - d / 0.06, 0, 1).astype(np.float32), (0, 0), 1.0)
        conf_prev = [ccol(src[i + 1], src[i]) for i in range(n - 1)]
        conf_next = [ccol(src[i], src[i + 1]) for i in range(n - 1)]
    else:
        f_next, f_prev, conf_prev, conf_next = None, None, None, None
    # 隣どうしのフローと信頼度
    f_next = f_next if f_next is not None else None
    if a.mode != "color":
      f_next = [flow(src[i], src[i + 1]) for i in range(n - 1)]   # i → i+1
      f_prev = [flow(src[i + 1], src[i]) for i in range(n - 1)]   # i+1 → i
      conf_prev = [confidence(src[i + 1], src[i], f_prev[i], f_next[i]) for i in range(n - 1)]  # i+1 から見た i
      conf_next = [confidence(src[i], src[i + 1], f_next[i], f_prev[i]) for i in range(n - 1)]  # i から見た i+1

    k = float(np.clip(a.keep, 0.0, 0.98))
    fwd = [depth[0]]
    for i in range(1, n):
        prev = warp(fwd[i - 1], f_prev[i - 1])
        wgt = k * conf_prev[i - 1]
        fwd.append(prev * wgt + depth[i] * (1 - wgt))
    bwd = [None] * n
    bwd[n - 1] = depth[n - 1]
    for i in range(n - 2, -1, -1):
        nxt = warp(bwd[i + 1], f_next[i])
        wgt = k * conf_next[i]
        bwd[i] = nxt * wgt + depth[i] * (1 - wgt)
    # 前向き・後ろ向きの平均 (どちらも今のコマを含むので、二重に数えた分を引く)
    out_dir = Path(a.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    for i in range(n):
        res = 0.5 * (fwd[i] + bwd[i])
        v = (np.clip(res, 0, 1) * scale + 0.5).astype(dtype)
        if raw[i].ndim == 3:
            v = np.dstack([v] * raw[i].shape[2])
        cv2.imwrite(str(out_dir / names[lo + i].name), v)
    print(f"保存: {out_dir}")


if __name__ == "__main__":
    main()
