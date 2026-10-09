#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_depth.py - 動画から深度の連番 PNG を作る (AE を使わずに試すため。AE の自動処理と同じ relight_depth.exe を使う)

  python tools/make_depth.py --video a.mp4 --out-dir cache/<名前>/depth [--provider cuda]
"""
import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from depth_test import DLL_DIRS, ENCODER, EXE, HEAD, ORT_CUDA, ORT_DML, decode, probe, write_frames  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--provider", default="cuda")
    ap.add_argument("--temporal", default="", help="時間方向のならし (0 で無し。空なら relight_depth の既定)")
    a = ap.parse_args()
    out = Path(a.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    w, h, fps = probe(a.video)
    frames = decode(a.video, w, h, 0)
    fpath = out.parent / "frames.bin"
    write_frames(fpath, frames, fps)
    print(f"フレーム {frames.shape[0]} 枚 {w}x{h} @ {fps:.2f}fps")
    ort = ORT_DML if a.provider == "dml" else ORT_CUDA
    cmd = [str(EXE), "--frames", str(fpath), "--out", str(out.parent / "depth.bin"), "--encoder", str(ENCODER), "--head", str(HEAD),
           "--provider", a.provider, "--ort", str(ort), "--png-dir", str(out)]
    if a.temporal:
        cmd += ["--temporal", a.temporal]
    for d in DLL_DIRS:
        cmd += ["--dll-dir", str(d)]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    print(r.stdout.strip()[-400:])
    if r.returncode != 0:
        print(r.stderr[-1500:])
        raise SystemExit(f"relight_depth が失敗 (code {r.returncode})")
    fpath.unlink(missing_ok=True)
    print(f"保存: {out} ({len(list(out.glob('*.png')))} 枚)")


if __name__ == "__main__":
    main()
