#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
depth_server.py - Relight Anime の深度だけを作る小さなサーバー (別の PC で動かす。ComfyUI 不要)

  POST /depth?model=vits|vitl&temporal=0.9&long_side=960   本文 = 動画ファイルそのもの
      → 200 application/octet-stream: numpy の npz (depth: uint16 [N,H,W] 近い = 65535、fps、range_source、model、temporal_keep)
  GET  /health → {"ok": true, "busy": false, ...}

手順 (relight_depth.exe と同じ考え方):
  動画のコマを取り出し、長辺 long_side に縮める → Video Depth Anything (公式の video_depth_anything と同じコード)
  → ショット全体の 2%〜98% で 0..1 に正規化 → 時間方向のならし (元の絵の色が変わっていない画素だけ前後のコマと混ぜる)
  → 16bit にして返す。

systemd のソケット起動で動かす: 依頼が来たときに起動し (LISTEN_FDS)、IDLE_EXIT 秒なにも来なければ自分で終わる
(待っている間はメモリも GPU も使わない)。LISTEN_FDS が無ければ --port で自分で待ち受ける (試験用)。
一度に 1 本だけ処理する (後から来た依頼は前の処理が終わるまで待つ)。
"""
import argparse
import io
import json
import os
import socket
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import cv2
import numpy as np
import torch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))   # video_depth_anything (公式のコード) をここに置く
from video_depth_anything.video_depth import VideoDepthAnything  # noqa: E402

MODEL_DIR = Path(os.environ.get("RELIGHT_MODEL_DIR", str(HERE.parent / "models")))
MODEL_FILES = {"vits": "video_depth_anything_vits.pth", "vitl": "video_depth_anything_vitl.pth"}
MODEL_CONFIGS = {
    "vits": {"encoder": "vits", "features": 64, "out_channels": [48, 96, 192, 384]},
    "vitl": {"encoder": "vitl", "features": 256, "out_channels": [256, 512, 1024, 1024]},
}
IDLE_EXIT = int(os.environ.get("RELIGHT_IDLE_EXIT", "300"))   # 秒。0 なら終わらない

_lock = threading.Lock()          # 一度に 1 本
_models = {}                      # 読み込んだモデル (起動している間だけ持つ)
_last_activity = time.time()
_busy = False


def log(msg):
    print(time.strftime("%H:%M:%S ") + msg, flush=True)


def load_model(name):
    if name not in _models:
        _models.clear()   # vits と vitl を同時には持たない (メモリを抑える)
        t0 = time.time()
        m = VideoDepthAnything(**MODEL_CONFIGS[name], metric=False)
        sd = torch.load(str(MODEL_DIR / MODEL_FILES[name]), map_location="cpu", weights_only=True)
        m.load_state_dict(sd, strict=True)
        _models[name] = m.to("cuda").eval()
        log(f"モデル {name} を読み込みました ({time.time() - t0:.1f}s)")
    return _models[name]


def read_frames(path, long_side):
    """動画の全コマを RGB uint8 で返す (長辺 long_side 以下に縮める、偶数の大きさ)"""
    cap = cv2.VideoCapture(str(path))
    fps = cap.get(cv2.CAP_PROP_FPS) or 24.0
    frames = []
    while True:
        ok, bgr = cap.read()
        if not ok:
            break
        h, w = bgr.shape[:2]
        if long_side > 0 and max(h, w) > long_side:
            s = long_side / max(h, w)
            nw, nh = max(2, int(round(w * s / 2)) * 2), max(2, int(round(h * s / 2)) * 2)
            bgr = cv2.resize(bgr, (nw, nh), interpolation=cv2.INTER_AREA)
        frames.append(cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB))
    cap.release()
    if not frames:
        raise ValueError("動画からコマを読めませんでした")
    return np.stack(frames), float(fps)


def normalize(depths, lo_pct=2.0, hi_pct=98.0):
    """ショット全体の 2%〜98% で 0..1 に (relight_depth.exe と同じ)"""
    sample = depths[:, ::4, ::4].ravel()
    lo, hi = np.percentile(sample, [lo_pct, hi_pct])
    hi = max(hi, lo + 1e-6)
    return np.clip((depths - lo) / (hi - lo), 0.0, 1.0).astype(np.float32), (float(lo), float(hi))


def temporal_smooth(depths, frames, keep):
    """
    時間方向のならし (relight_depth.exe の temporalSmooth と同じ)。
    元の絵の色が変わっていない画素だけ、前 (後ろ) のならした深度を keep の割合で残し、前向きと後ろ向きを平均する。
    動いている所は色が変わるので混ぜない (残像にならない)。GPU で計算する。
    """
    n = depths.shape[0]
    if n < 2 or keep <= 0:
        return depths
    dev = "cuda"
    d = torch.from_numpy(depths).to(dev)
    g = torch.from_numpy(frames).to(dev)

    def conf(i, j):
        diff = (g[i].int() - g[j].int()).abs().amax(dim=2).float() / 255.0
        c = (1.0 - diff / 0.06).clamp(0.0, 1.0)[None, None]
        return torch.nn.functional.avg_pool2d(c, 3, stride=1, padding=1, count_include_pad=False)[0, 0]

    fwd = torch.empty_like(d)
    acc = d[0].clone()
    fwd[0] = acc
    for i in range(1, n):
        w = keep * conf(i, i - 1)
        acc = acc * w + d[i] * (1.0 - w)
        fwd[i] = acc
    out = torch.empty_like(d)
    acc = d[n - 1].clone()
    out[n - 1] = 0.5 * (fwd[n - 1] + acc)
    for i in range(n - 2, -1, -1):
        w = keep * conf(i, i + 1)
        acc = acc * w + d[i] * (1.0 - w)
        out[i] = 0.5 * (fwd[i] + acc)
    return out.cpu().numpy()


class _NoBar:
    def update(self, n):
        pass


def make_depth(video_path, model_name, temporal, long_side):
    t0 = time.time()
    frames, fps = read_frames(video_path, long_side)
    t1 = time.time()
    model = load_model(model_name)
    depths = model.infer_video_depth(frames, input_size=518, device="cuda", pbar=_NoBar(), fp32=False)
    depths = np.asarray(depths, dtype=np.float32)
    t2 = time.time()
    depths, rng = normalize(depths)
    depths = temporal_smooth(depths, frames, temporal)
    u16 = (np.clip(depths, 0, 1) * 65535.0 + 0.5).astype(np.uint16)
    t3 = time.time()
    log(f"{len(frames)} コマ {frames.shape[2]}x{frames.shape[1]} {model_name}: 読み込み {t1 - t0:.1f}s 推定 {t2 - t1:.1f}s 後処理 {t3 - t2:.1f}s")
    buf = io.BytesIO()
    np.savez_compressed(buf, depth=u16, fps=np.float32(fps), range_source=np.array(rng, np.float32),
                        model=np.array(MODEL_FILES[model_name]), temporal_keep=np.float32(temporal))
    torch.cuda.empty_cache()
    return buf.getvalue()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        log("http " + (fmt % args))

    def _send(self, code, body, ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _error(self, code, msg):
        self._send(code, json.dumps({"error": msg}, ensure_ascii=False).encode("utf-8"))

    def do_GET(self):
        global _last_activity
        _last_activity = time.time()
        if urlparse(self.path).path == "/health":
            self._send(200, json.dumps({"ok": True, "busy": _busy, "models": list(_models),
                                        "cuda": torch.cuda.is_available()}).encode("utf-8"))
        else:
            self._error(404, "not found")

    def do_POST(self):
        global _last_activity, _busy
        _last_activity = time.time()
        u = urlparse(self.path)
        if u.path != "/depth":
            return self._error(404, "not found")
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        model_name = q.get("model", "vits")
        if model_name not in MODEL_FILES:
            return self._error(400, f"model は {list(MODEL_FILES)} のどれか")
        try:
            temporal = float(q.get("temporal", "0.9"))
            long_side = int(q.get("long_side", "960"))
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            return self._error(400, "引数が不正です")
        if length <= 0:
            return self._error(400, "動画が空です")
        suffix = Path(q.get("name", "video.mp4")).suffix or ".mp4"
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / ("input" + suffix)
            with open(path, "wb") as f:
                left = length
                while left > 0:
                    chunk = self.rfile.read(min(left, 1 << 20))
                    if not chunk:
                        break
                    f.write(chunk)
                    left -= len(chunk)
            if left > 0:
                return self._error(400, "動画を最後まで受け取れませんでした")
            with _lock:
                _busy = True
                try:
                    body = make_depth(path, model_name, temporal, long_side)
                except Exception as e:  # noqa: BLE001
                    log(f"失敗: {type(e).__name__}: {e}")
                    return self._error(500, f"{type(e).__name__}: {e}")
                finally:
                    _busy = False
                    _last_activity = time.time()
        self._send(200, body, "application/octet-stream")


def idle_watch(server):
    while True:
        time.sleep(15)
        if IDLE_EXIT > 0 and not _busy and time.time() - _last_activity > IDLE_EXIT:
            log(f"{IDLE_EXIT}s 依頼が無いので終わります")
            server.shutdown()
            return


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8191, help="LISTEN_FDS が無いとき (試験用) に待ち受けるポート")
    a = ap.parse_args()
    if os.environ.get("LISTEN_FDS") == "1" and os.environ.get("LISTEN_PID") == str(os.getpid()):
        # systemd のソケット起動: ソケットは fd 3 で渡される
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler, bind_and_activate=False)
        server.socket.close()
        server.socket = socket.socket(fileno=3)
        server.server_address = server.socket.getsockname()
        log(f"ソケット起動 {server.server_address}")
    else:
        server = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
        log(f"待ち受け 127.0.0.1:{a.port}")
    server.daemon_threads = True
    threading.Thread(target=idle_watch, args=(server,), daemon=True).start()
    server.serve_forever()


if __name__ == "__main__":
    main()
