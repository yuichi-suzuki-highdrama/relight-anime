#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
inspect_exr.py - 生成した EXR の中身を確認する (チャンネル、サイズ、値域、中央と四隅の値)
  python tools/inspect_exr.py cache/shot01/normal/normal.00001.exr cache/shot01/depth/depth.00001.exr
"""
import sys

import numpy as np


def main():
    try:
        import OpenEXR
    except ImportError:
        raise SystemExit("pip install OpenEXR が必要です")
    for path in sys.argv[1:]:
        with OpenEXR.File(path) as f:
            hdr = f.header()
            chans = f.channels()
            print(f"{path}")
            print(f"  compression={hdr.get('compression')} channels={list(chans.keys())}")
            for name, ch in chans.items():
                arr = np.asarray(ch.pixels).astype(np.float32)
                if arr.ndim == 3:
                    h, w, c = arr.shape
                    print(f"  {name}: {w}x{h}x{c}")
                    for i in range(c):
                        a = arr[:, :, i]
                        print(f"    ch{i}: min={a.min():.4f} max={a.max():.4f} mean={a.mean():.4f} "
                              f"center={a[h//2, w//2]:.4f} tl={a[0,0]:.4f} br={a[-1,-1]:.4f}")
                else:
                    h, w = arr.shape
                    print(f"  {name}: {w}x{h} min={arr.min():.4f} max={arr.max():.4f} mean={arr.mean():.4f} "
                          f"center={arr[h//2, w//2]:.4f} tl={arr[0,0]:.4f} br={arr[-1,-1]:.4f}")


if __name__ == "__main__":
    main()
