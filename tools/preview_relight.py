#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
preview_relight.py - プラグイン (RelightAE.cpp) と同じ照明式の Python 参照実装。
AE SDK 無しで照明式と座標規約を検証し、1 フレームのプレビュー PNG を作る。

  python tools/preview_relight.py --shot test_anime_dance --frame 1 --out preview.png
  python tools/preview_relight.py --shot test_anime_dance --frame 1 --az 135 --el 30 --toon 3 --rim 0.6

出力 PNG は左から: 元画像 / 法線 / 深度 / 再照明結果 / ライトパスのみ
"""
import argparse
import json
from pathlib import Path

import numpy as np

try:
    import OpenEXR
except ImportError:
    raise SystemExit("pip install OpenEXR が必要です")
try:
    from PIL import Image
except ImportError:
    raise SystemExit("pip install pillow が必要です")

ROOT = Path(__file__).resolve().parent.parent


def read_pass(shot_dir, sub, frame):
    """normal/depth のパスを読む (png16 か exr)。HxWx3 float32 0..1"""
    png = shot_dir / sub / f"{sub}.{frame:05d}.png"
    exr = shot_dir / sub / f"{sub}.{frame:05d}.exr"
    if png.exists():
        import struct
        with open(png, "rb") as f:
            bd = struct.unpack(">IIBB", f.read(26)[16:26])[2]
        if bd == 16:
            try:
                import cv2
                arr = cv2.imread(str(png), cv2.IMREAD_UNCHANGED)[:, :, ::-1].astype(np.float32) / 65535.0
                return arr
            except ImportError:
                pass  # OpenCV が無ければ PIL (8bit に落ちる)
        return np.asarray(Image.open(png).convert("RGB")).astype(np.float32) / 255.0
    return read_exr_rgb(exr)


def read_exr_rgb(path):
    with OpenEXR.File(str(path)) as f:
        ch = f.channels()
        if "RGB" in ch:
            arr = np.asarray(ch["RGB"].pixels).astype(np.float32)
        elif "RGBA" in ch:
            arr = np.asarray(ch["RGBA"].pixels).astype(np.float32)[:, :, :3]
        else:
            r = np.asarray(ch["R"].pixels); g = np.asarray(ch["G"].pixels); b = np.asarray(ch["B"].pixels)
            arr = np.stack([r, g, b], axis=-1).astype(np.float32)
    return arr


def extract_frame(video, index, out_png):
    """ffmpeg で index (1 始まり) のフレームを PNG に出す"""
    import subprocess
    cmd = ["ffmpeg", "-y", "-v", "error", "-i", str(video), "-vf", f"select=eq(n\\,{index-1})",
           "-vframes", "1", str(out_png)]
    subprocess.run(cmd, check=True)


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(c, 0, None)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)


def toon_quantize(d, steps, softness):
    if steps <= 0:
        return d
    t = d * steps
    i = np.floor(t)
    f = t - i
    half = np.clip(softness, 0, 1) * 0.5
    if half <= 1e-4:
        fs = (f >= 0.5).astype(np.float32)
    else:
        x = np.clip((f - (0.5 - half)) / (2 * half), 0, 1)
        fs = x * x * (3 - 2 * x)
    return np.clip((i + fs) / steps, 0, 1)


def relight(src, normal_exr, depth_exr, a):
    """src: HxWx3 (0..1 sRGB), normal_exr: HxWx3 (0..1), depth_exr: HxWx3 (0..1, 近い=白)"""
    H, W = src.shape[:2]
    # 法線 OpenGL → AE 空間 (x右, y下, z奥)
    n = normal_exr * 2 - 1
    N = np.stack([n[:, :, 0], -n[:, :, 1], -n[:, :, 2]], axis=-1)
    N /= np.maximum(np.linalg.norm(N, axis=-1, keepdims=True), 1e-8)

    # 深度 → z (px)
    d = depth_exr[:, :, 0]
    z = (1 - d) * a.depth_scale
    ys, xs = np.mgrid[0:H, 0:W].astype(np.float32)
    P = np.stack([xs, ys, z], axis=-1)

    V = np.array([0, 0, -1], np.float32)
    NdotV = np.clip((N * V).sum(-1), 0, 1)

    shade = np.full((H, W, 3), a.ambient, np.float32)
    spec = np.zeros((H, W, 3), np.float32)
    color = np.array(a.color, np.float32) / 255.0

    if a.light_type == "point":
        Lp = np.array([a.pos[0], a.pos[1], a.pos[2]], np.float32)
        Lv = Lp[None, None, :] - P
        dist2 = (Lv * Lv).sum(-1)
        Lv /= np.maximum(np.sqrt(dist2)[..., None], 1e-8)
        atten = 1.0 / (1.0 + dist2 / (a.radius ** 2))
    else:
        az = np.deg2rad(a.az); el = np.deg2rad(a.el)
        Ld = np.array([np.cos(az) * np.cos(el), -np.sin(az) * np.cos(el), -np.sin(el)], np.float32)
        Ld /= np.linalg.norm(Ld)
        Lv = np.broadcast_to(Ld, (H, W, 3))
        atten = np.ones((H, W), np.float32)

    diff = toon_quantize(np.clip((N * Lv).sum(-1), 0, 1), a.toon, a.toon_soft)
    k = (a.intensity * atten * diff)[..., None]
    shade += color * k

    if a.specular > 0:
        Hv = Lv + V
        Hv = Hv / np.maximum(np.linalg.norm(Hv, axis=-1, keepdims=True), 1e-8)
        s = np.power(np.clip((N * Hv).sum(-1), 0, 1), max(1.0, a.shininess))
        s = toon_quantize(s, a.toon, a.toon_soft) * a.specular * atten
        spec += color * s[..., None]

    if a.rim > 0:
        r = np.power(1 - NdotV, max(0.1, a.rim_width))
        r = toon_quantize(r, a.toon, a.toon_soft) * a.rim
        spec += (np.array(a.rim_color, np.float32) / 255.0) * r[..., None]

    s = srgb_to_linear(src) if a.linearize else src
    lit = s * shade + spec
    light_pass = np.clip(s * (shade - 1) + spec, 0, None)
    if a.linearize:
        lit = linear_to_srgb(lit)
        light_pass = linear_to_srgb(light_pass)
    return np.clip(lit, 0, 1), np.clip(light_pass, 0, 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shot", required=True)
    ap.add_argument("--cache", default=str(ROOT / "cache"))
    ap.add_argument("--frame", type=int, default=1, help="1 始まり")
    ap.add_argument("--out", default=None)
    ap.add_argument("--light-type", choices=["directional", "point"], default="directional")
    ap.add_argument("--az", type=float, default=45.0)
    ap.add_argument("--el", type=float, default=35.0)
    ap.add_argument("--pos", type=float, nargs=3, default=[240, 300, -500])
    ap.add_argument("--radius", type=float, default=600.0)
    ap.add_argument("--color", type=float, nargs=3, default=[255, 240, 220])
    ap.add_argument("--intensity", type=float, default=0.6)
    ap.add_argument("--ambient", type=float, default=1.0)
    ap.add_argument("--specular", type=float, default=0.0)
    ap.add_argument("--shininess", type=float, default=32.0)
    ap.add_argument("--rim", type=float, default=0.0)
    ap.add_argument("--rim-width", type=float, default=3.0)
    ap.add_argument("--rim-color", type=float, nargs=3, default=[200, 220, 255])
    ap.add_argument("--toon", type=int, default=0)
    ap.add_argument("--toon-soft", type=float, default=0.1)
    ap.add_argument("--depth-scale", type=float, default=1000.0)
    ap.add_argument("--linearize", action="store_true")
    a = ap.parse_args()

    shot_dir = Path(a.cache) / a.shot
    manifest = json.loads((shot_dir / "manifest.json").read_text(encoding="utf-8"))
    normal = read_pass(shot_dir, "normal", a.frame)
    depth = read_pass(shot_dir, "depth", a.frame)

    src_png = shot_dir / f"src.{a.frame:05d}.png"
    if not src_png.exists():
        src_path = Path(manifest["source"])
        if src_path.suffix.lower() in {".png", ".jpg", ".jpeg", ".webp", ".tif", ".tiff", ".bmp"}:
            Image.open(src_path).convert("RGB").save(src_png)   # 静止画はそのまま
        else:
            extract_frame(manifest["source"], a.frame + manifest.get("skip", 0), src_png)
    src = np.asarray(Image.open(src_png).convert("RGB")).astype(np.float32) / 255.0
    if src.shape[:2] != normal.shape[:2]:
        src = np.asarray(Image.fromarray((src * 255).astype(np.uint8)).resize((normal.shape[1], normal.shape[0]))).astype(np.float32) / 255.0

    lit, light_pass = relight(src, normal, depth, a)

    panels = [src, np.clip(normal, 0, 1), np.clip(depth, 0, 1), lit, light_pass]
    strip = np.concatenate(panels, axis=1)
    out = Path(a.out) if a.out else shot_dir / f"preview.{a.frame:05d}.png"
    Image.fromarray((strip * 255 + 0.5).astype(np.uint8)).save(out)
    print(f"保存: {out}  (元 / 法線 / 深度 / 再照明 / ライトパス)")


if __name__ == "__main__":
    main()
