#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
compare_presets.py - 1 フレームを各プリセット (または指定の設定) で再照明し、並べた画像を作る

  python tools/harness/compare_presets.py --video a.mp4 --frame 167 --depth cache/x/depth/depth.00168.png --out grid.png
  python tools/harness/compare_presets.py ... --sets "元=" "影色なし=tint=0" "セル=cel=1 steps=1"

--sets を省くと、元の絵 + プリセット全部。各設定は「名前=key=value key=value」。
"""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_cli import load_src, load_png16, run  # noqa: E402

PRESETS = ["Anime Standard", "Soft Daylight", "Sunset Backlight", "Night Neon", "Cel Hard Light", "Cinematic (TypeGPU)"]


def label(img, text):
    im = Image.fromarray(np.ascontiguousarray(img))
    d = ImageDraw.Draw(im)
    try:
        font = ImageFont.truetype("C:/Windows/Fonts/YuGothB.ttc", max(16, im.height // 24))
    except OSError:
        font = ImageFont.load_default()
    d.rectangle([0, 0, im.width, font.size + 12], fill=(0, 0, 0))
    d.text((8, 4), text, fill=(255, 255, 255), font=font)
    return np.asarray(im)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src")
    ap.add_argument("--video")
    ap.add_argument("--frame", type=int, default=0)
    ap.add_argument("--depth", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--cols", type=int, default=3)
    ap.add_argument("--crop", help="x0,y0,x1,y1 (各結果をこの範囲で切り出して並べる)")
    ap.add_argument("--common", default="", help="全部に付ける key=value (空白区切り)")
    ap.add_argument("--sets", nargs="*")
    a = ap.parse_args()

    src = load_src(a)
    H, W = src.shape[:2]
    d = load_png16(a.depth)
    if d.ndim == 3:
        d = d[:, :, 0]

    common = a.common.split()
    if a.sets:
        sets = []
        for s in a.sets:
            name, _, kv = s.partition("=")
            sets.append((name, kv.split() if kv else None))
    else:
        sets = [("元の絵", None)] + [(f"{i}: {n}", [f"preset={i}"]) for i, n in enumerate(PRESETS)]

    tiles = []
    for name, kv in sets:
        if kv is None and name == "元の絵":
            img = src[:, :, :3]
        else:
            mode = 1
            extra = list(common) + list(kv or [])
            for e in extra:
                if e.startswith("mode="):
                    mode = int(e[5:])
            img = run(src, d, [e for e in extra if not e.startswith("mode=")], mode=mode)[:, :, :3]
        if a.crop:
            x0, y0, x1, y1 = (int(v) for v in a.crop.split(","))
            img = img[y0:y1, x0:x1]
        tiles.append(label((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8), name))

    cols = a.cols
    rows = (len(tiles) + cols - 1) // cols
    blank = np.zeros_like(tiles[0])
    tiles += [blank] * (rows * cols - len(tiles))
    grid = np.concatenate([np.concatenate(tiles[r * cols:(r + 1) * cols], axis=1) for r in range(rows)], axis=0)
    Image.fromarray(grid).save(a.out)
    print(f"保存: {a.out}")


if __name__ == "__main__":
    main()
