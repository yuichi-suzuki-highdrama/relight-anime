#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
flow_flicker.py - 動きを差し引いた「本当の揺らぎ」を測る

  python tools/harness/flow_flicker.py --video a.mp4 --depth-dir cache/x/depth --start 100 --count 16 [--extra "..."] [--sets parts]

flicker_test.py は同じ画素どうしを比べるので、キャラが動いた分の正当な変化も揺らぎに数えてしまう。
ここでは元の絵からオプティカルフロー (DIS) を求め、前のコマを今のコマの位置へずらしてから比べる。
フローが信頼できる画素 (前後のフローが打ち消し合い、ずらした元の絵の色が合う所) だけで平均する。

出す値:
  元の絵   : ずらした前のコマと今のコマの差 (フローの精度の目安。これより小さい揺らぎは測れない)
  深度     : 深度の、動きに沿った変化
  効果     : 照明の効果 (照明後 − 元の絵) の、動きに沿った変化 = 見た目の揺らぎ
"""
import argparse
import sys
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from flicker_test import frames  # noqa: E402
from run_cli import load_png16, run  # noqa: E402


def gray(img):
    return cv2.cvtColor((np.clip(img[:, :, :3], 0, 1) * 255).astype(np.uint8), cv2.COLOR_RGB2GRAY)


def flow(a, b):
    """a から b へのフロー (a の各画素が b のどこへ動いたか)"""
    dis = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
    return dis.calc(gray(a), gray(b), None)


def warp(img, fl):
    """img (前のコマ) を、今のコマの位置へずらす。fl は今のコマから前のコマへのフロー"""
    h, w = fl.shape[:2]
    gx, gy = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
    return cv2.remap(img, gx + fl[:, :, 0], gy + fl[:, :, 1], cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)


def valid_mask(cur, prev, f_cp, f_pc, warped_src):
    """フローが信頼できる画素: 行って戻るとほぼ元の位置 (1 画素以内)、ずらした色が合う"""
    back = warp(f_pc, f_cp)
    fb = np.linalg.norm(f_cp + back, axis=2) < 1.0
    col = np.abs(warped_src[:, :, :3] - cur[:, :, :3]).max(axis=2) < 0.06
    return fb & col


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", required=True)
    ap.add_argument("--depth-dir", required=True)
    ap.add_argument("--start", type=int, default=100)
    ap.add_argument("--count", type=int, default=16)
    ap.add_argument("--extra", default="")
    ap.add_argument("--sets", default="base", help="base / parts / source / shadow")
    ap.add_argument("--radius", type=int, default=2, help="深度を時間方向にならす前後のフレーム数 (既定の作りと同じ 2)")
    ap.add_argument("--scale", type=float, default=1.0, help="元の絵を縮めて描く (AE の 1/2・1/4 画質のプレビューを真似る)")
    ap.add_argument("--depth-shift", type=int, default=0, help="深度を何コマずらして使うか (ずれたときの揺らぎを見る)")
    ap.add_argument("--blur", type=float, default=0.0, help="比べる前にぼかす (画素。広い範囲のゆっくりした揺れだけを見る)")
    a = ap.parse_args()

    srcs = frames(a.video, a.start, a.count)
    if a.scale != 1.0:
        srcs = [cv2.resize(s, (int(s.shape[1] * a.scale), int(s.shape[0] * a.scale)), interpolation=cv2.INTER_AREA) for s in srcs]

    def depth_at(n):
        n = n + a.depth_shift
        p = Path(a.depth_dir) / f"depth.{n + 1:05d}.png"
        if n < 0 or not p.exists():
            return None
        d = load_png16(p)
        return d[:, :, 0] if d.ndim == 3 else d

    depths = [depth_at(a.start + i) for i in range(len(srcs))]
    h, w = srcs[0].shape[:2]

    def neighbors(i, radius):
        out = []
        for off in [-1, 1, -2, 2, -3, 3, -4, 4][: radius * 2]:
            nd = depth_at(a.start + i + off)
            if nd is not None:
                out.append((off, nd))
        return out

    # フロー (今 → 前、前 → 今) と、信頼できる画素
    pairs = []
    src_err = []
    for i in range(1, len(srcs)):
        f_cp = flow(srcs[i], srcs[i - 1])
        f_pc = flow(srcs[i - 1], srcs[i])
        ws = warp(srcs[i - 1], f_cp)
        m = valid_mask(srcs[i], srcs[i - 1], f_cp, f_pc, ws)
        pairs.append((f_cp, m))
        src_err.append(np.abs(cv2.GaussianBlur(ws[:, :, :3], (0, 0), a.blur) - cv2.GaussianBlur(srcs[i][:, :, :3], (0, 0), a.blur)).max(axis=2)[m].mean() if a.blur > 0 else np.abs(ws[:, :, :3] - srcs[i][:, :, :3]).max(axis=2)[m].mean())
    cover = np.mean([m.mean() for _, m in pairs])
    print(f"フローが信頼できる画素の割合: {cover:.2f}")
    print(f"元の絵 (フローの精度の目安)   : {np.mean(src_err):.5f}")

    def lo(img):
        return cv2.GaussianBlur(img, (0, 0), a.blur) if a.blur > 0 else img

    def dres(d):
        return cv2.resize(d, (w, h), interpolation=cv2.INTER_LINEAR) if d.shape != (h, w) else d

    dz = [dres(d) for d in depths]
    dep = np.mean([np.abs(lo(warp(dz[i - 1], f)) - lo(dz[i]))[m].mean() for i, (f, m) in enumerate(pairs, start=1)])
    print(f"深度の動きに沿った変化       : {dep:.5f}")

    base = a.extra.split() + ["gpu=1"]
    sets = [("今の既定", [], a.radius)]
    if a.sets == "parts":
        sets += [
            ("ならし無し", [], 0),
            ("落ちる影なし", ["shadow=0"], a.radius),
            ("セル調なし", ["cel=0"], a.radius),
            ("立体の混ぜなし (form=0)", ["form=0"], a.radius),
            ("リムなし", ["rim=0"], a.radius),
            ("光 (影色・リムだけ)", ["intensity=0.01"], a.radius),
        ]
    if a.sets == "shadow":
        sets += [
            ("ならし無し", [], 0),
            ("落ちる影なし", ["shadow=0"], a.radius),
            ("落ちる影 半分", ["shadow=0.5"], a.radius),
            ("セル調なし", ["cel=0"], a.radius),
            ("リムなし", ["rim=0"], a.radius),
        ]
    if a.sets == "source":
        sets += [
            ("膨らみなし (volume=0)", ["volume=0"], a.radius),
            ("線画合わせなし (snap=0)", ["snap=0"], a.radius),
            ("線画の保護なし (line=0)", ["line=0"], a.radius),
            ("影色なし (tint=0)", ["tint=0"], a.radius),
            ("なめらかさ 最大 (smooth=1)", ["smooth=1"], a.radius),
            ("セル調なし・膨らみなし", ["cel=0", "volume=0"], a.radius),
        ]
    print(f"{'設定':<28} {'見た目の揺らぎ':>12}")
    for name, kv, radius in sets:
        effects = []
        for i, (s, d) in enumerate(zip(srcs, depths)):
            out = run(s, d, base + kv, mode=1, neighbors=neighbors(i, radius))[:, :, :3]
            effects.append(out - s[:, :, :3])
        diffs = [np.abs(lo(warp(effects[i - 1], f)) - lo(effects[i])).max(axis=2)[m] for i, (f, m) in enumerate(pairs, start=1)]
        fl = np.mean([d.mean() for d in diffs])
        p99 = np.max([np.percentile(d, 99) for d in diffs])
        print(f"{name:<28} {fl:12.5f}  最大 {p99:.4f}")


if __name__ == "__main__":
    main()
