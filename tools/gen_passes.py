#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_passes.py - ComfyUI API で動画 (または静止画) から法線・深度パスを生成し、EXR 連番としてキャッシュする。

  python tools/gen_passes.py --video "C:/path/shot.mp4" --shot shot01
  python tools/gen_passes.py --video shot.mp4 --shot shot01 --frame-cap 48   # 試し用に先頭 48 フレーム

出力 (既定 cache/<shot>/):
  normal/normal.00001.exr   MoGe-2 法線 (OpenGL 規約, 0..1 エンコード, 16bit float, 線形書き出し)
  depth/depth.00001.exr     Video Depth Anything 深度 (ショット全体で 0..1 正規化, 近い=白)
  depth_raw/frame_00000.exr Video Depth Anything 生の逆深度 (Z チャンネルのみ, 32bit float)
  manifest.json             生成条件の記録

前提: ComfyUI が稼働中で、以下のノードが使えること
  VHS_LoadVideoPath / LoadMoGeModel / MoGeInference / MoGeRender / SaveImageAdvanced
  LoadVideoDepthAnythingModel / VideoDepthAnythingProcess / VideoDepthAnythingOutput / VideoDepthAnythingSaveEXR

--remote: 別の PC の深度サーバー (tools/depth_server/depth_server.py) で深度だけ作る。
  ComfyUI は使わない。動画を POST /depth で送り、範囲の正規化と時間方向のならしまで済んだ 16bit の深度を受け取る。
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from pathlib import Path

DEFAULT_SERVER = os.environ.get("COMFY_SERVER", "http://127.0.0.1:8188")
DEFAULT_COMFY_OUTPUT = os.environ.get("COMFY_OUTPUT", r"C:\ComfyUI_windows_portable\ComfyUI\output")
DEFAULT_CACHE = Path(__file__).resolve().parent.parent / "cache"

NORMAL_MODEL = "moge_2_vitl_normal_fp16.safetensors"
DEPTH_MODEL = "video_depth_anything_vitl.pth"


def log(msg):
    print(msg, flush=True)


def file_sha1(path, limit=64 * 1024 * 1024):
    """先頭 limit バイト + サイズで素材を識別する (巨大動画の全量ハッシュは避ける)"""
    h = hashlib.sha1()
    p = Path(path)
    h.update(str(p.stat().st_size).encode())
    with open(p, "rb") as f:
        h.update(f.read(limit))
    return h.hexdigest()[:16]


IMAGE_EXTS = {".png", ".jpg", ".jpeg", ".webp", ".tif", ".tiff", ".bmp"}


def is_image(path):
    return Path(path).suffix.lower() in IMAGE_EXTS


def build_workflow(a, job):
    g = {}
    if is_image(a.video):
        # 静止画 (1 フレーム扱い)
        g["load"] = {
            "class_type": "VHS_LoadImagePath",
            "inputs": {"image": a.video, "custom_width": 0, "custom_height": 0},
        }
    else:
        g["load"] = {
            "class_type": "VHS_LoadVideoPath",
            "inputs": {
                "video": a.video,
                "force_rate": a.force_rate,
                "custom_width": 0,
                "custom_height": 0,
                "frame_load_cap": a.frame_cap,
                "skip_first_frames": a.skip,
                "select_every_nth": a.every_nth,
                "format": "None",
            },
        }
    # SaveImageAdvanced の format は動的コンボ。子入力は "format.<名前>" で渡す
    if a.format == "exr":
        exr_save = {
            "format": "exr",
            "format.bit_depth": "16-bit float",
            "format.input_color_space": "linear",  # データなので色変換せず書き出す
        }
    else:
        # 16bit PNG: AE が色管理オフなら無変換で読める (EXR は Preserve RGB 無しだと変換される)
        exr_save = {
            "format": "png",
            "format.bit_depth": "16-bit",
            "format.input_color_space": "sRGB",
        }
    if not a.no_normal:
        g["moge_model"] = {"class_type": "LoadMoGeModel", "inputs": {"model_name": a.normal_model}}
        g["moge"] = {
            "class_type": "MoGeInference",
            "inputs": {
                "moge_model": ["moge_model", 0],
                "image": ["load", 0],
                "resolution_level": a.moge_level,
                "fov_x_degrees": 0.0,
                "batch_size": a.batch,
                "force_projection": True,
                "apply_mask": False,
                "refine_steps": 0,
                "mode": "mono",
            },
        }
        g["normal_img"] = {
            "class_type": "MoGeRender",
            "inputs": {"moge_geometry": ["moge", 0], "output": "normal_opengl"},
        }
        g["normal_save"] = {
            "class_type": "SaveImageAdvanced",
            "inputs": dict(images=["normal_img", 0], filename_prefix=f"{job}/normal/normal", **exr_save),
        }
    if not a.no_depth:
        g["vda_model"] = {"class_type": "LoadVideoDepthAnythingModel", "inputs": {"model": a.depth_model}}
        g["vda"] = {
            "class_type": "VideoDepthAnythingProcess",
            "inputs": {
                "vda_model": ["vda_model", 0],
                "images": ["load", 0],
                "input_size": a.vda_input_size,
                "max_res": a.vda_max_res,
                "precision": "fp16",
            },
        }
        g["depth_img"] = {
            "class_type": "VideoDepthAnythingOutput",
            "inputs": {"depths": ["vda", 0], "colormap": "gray"},
        }
        g["depth_save"] = {
            "class_type": "SaveImageAdvanced",
            "inputs": dict(images=["depth_img", 0], filename_prefix=f"{job}/depth/depth", **exr_save),
        }
        if a.depth_raw:
            g["depth_raw_save"] = {
                "class_type": "VideoDepthAnythingSaveEXR",
                "inputs": {"depths": ["vda", 0], "folder_name": f"{job}/depth_raw"},
            }
    return g


def api(server, path, data=None, timeout=60):
    url = server.rstrip("/") + path
    body = None
    headers = {}
    if data is not None:
        body = json.dumps(data).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=body, headers=headers)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def run_remote(a):
    """
    別の PC の深度サーバー (tools/depth_server/depth_server.py) で深度を作る。ComfyUI は使わない。
    動画を送ると、範囲の正規化と時間方向のならしまで済んだ 16bit の深度 (npz) が返る。それを連番 PNG にする
    """
    import io
    import cv2
    import numpy as np

    shot = re.sub(r"[^\w\-]+", "_", a.shot)
    dest = Path(a.cache) / shot
    (dest / "depth").mkdir(parents=True, exist_ok=True)

    def write_status(state, message=""):
        (dest / "status.json").write_text(
            json.dumps({"state": state, "message": message, "time": time.strftime("%Y-%m-%dT%H:%M:%S"),
                        "pid": os.getpid()}, ensure_ascii=False), encoding="utf-8")

    model = "vitl" if "vitl" in a.depth_model else "vits"
    t0 = time.time()
    try:
        write_status("running", f"深度サーバーで推定中 ({model})")
        q = urllib.parse.urlencode({"model": model, "temporal": a.temporal, "long_side": a.remote_long_side,
                                    "name": Path(a.video).name})
        body = Path(a.video).read_bytes()
        log(f"深度サーバーへ送信: {a.server} ({len(body) / 1e6:.1f}MB, {model})")
        req = urllib.request.Request(a.server.rstrip("/") + "/depth?" + q, data=body,
                                     headers={"Content-Type": "application/octet-stream"})
        try:
            with urllib.request.urlopen(req, timeout=3600) as r:
                data = r.read()
        except urllib.error.HTTPError as e:
            raise SystemExit(f"深度サーバーで失敗しました ({e.code}): {e.read().decode('utf-8', 'replace')[:2000]}")
        except urllib.error.URLError as e:
            raise SystemExit(f"深度サーバーに接続できません ({a.server}): {e.reason}")
        log(f"受信しました ({time.time() - t0:.1f}s, {len(data) / 1e6:.1f}MB)")
        write_status("running", "深度を書き出し中")
        z = np.load(io.BytesIO(data))
        depth = z["depth"]
        for old in (dest / "depth").glob("depth.*.png"):
            old.unlink()
        for i in range(depth.shape[0]):
            cv2.imwrite(str(dest / "depth" / f"depth.{i + 1:05d}.png"), depth[i])
    except SystemExit as e:
        write_status("error", str(e))
        raise
    except Exception as e:  # noqa: BLE001
        write_status("error", f"{type(e).__name__}: {e}")
        raise
    n, h, w = depth.shape
    manifest = {
        "generator": "relight_depth_server",
        "format": "png16",
        "fps": float(z["fps"]),
        "frames": {"depth": int(n)},
        "source": a.video,
        "source_id": file_sha1(a.video),
        "server": a.server,
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "depth": {"model": str(z["model"]), "near": "white", "normalization": "shot-global 2-98 percentile",
                  "temporal_keep": float(z["temporal_keep"]), "range_applied": True,
                  "range_source": [float(v) for v in z["range_source"]], "size": [int(w), int(h)],
                  "file": "depth/depth.%05d.png"},
        "normal": None,
        "elapsed_sec": round(time.time() - t0, 1),
    }
    (dest / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    write_status("done", f"frames={n}")
    log(f"完了: {dest} ({n} コマ, {w}x{h}, {time.time() - t0:.1f}s)")


def submit(server, graph):
    client_id = uuid.uuid4().hex
    try:
        res = api(server, "/prompt", {"prompt": graph, "client_id": client_id})
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")
        raise SystemExit(f"ComfyUI がワークフローを拒否しました ({e.code}):\n{detail}")
    if "error" in res and res["error"]:
        raise SystemExit(f"ComfyUI エラー: {json.dumps(res, ensure_ascii=False, indent=2)}")
    # 一部ノードだけ検証に落ちた場合、ComfyUI は 200 を返して残りを実行する。
    # パスが欠けたまま進むのを防ぐため、ここで止める。
    if res.get("node_errors"):
        raise SystemExit("ComfyUI が一部ノードを拒否しました:\n"
                         + json.dumps(res["node_errors"], ensure_ascii=False, indent=2)[:4000])
    return res["prompt_id"]


def wait(server, prompt_id, poll=2.0):
    t0 = time.time()
    last = ""
    while True:
        hist = api(server, f"/history/{prompt_id}")
        if prompt_id in hist:
            entry = hist[prompt_id]
            status = entry.get("status", {})
            if status.get("completed") or status.get("status_str") in ("success", "error"):
                return entry
        try:
            q = api(server, "/queue")
            running = len(q.get("queue_running", []))
            pending = len(q.get("queue_pending", []))
            msg = f"  実行中 {running} / 待機 {pending} / 経過 {int(time.time()-t0)}s"
        except Exception:
            msg = f"  経過 {int(time.time()-t0)}s"
        if msg != last:
            log(msg)
            last = msg
        time.sleep(poll)


def collect(comfy_output, job, dest, keep_comfy_copy=False):
    """ComfyUI/output/<job>/ 配下を dest へ移し、AE 向けの連番名に揃える"""
    src_root = Path(comfy_output) / job
    if not src_root.exists():
        raise SystemExit(f"出力フォルダが見つかりません: {src_root}")
    dest.mkdir(parents=True, exist_ok=True)
    counts = {}
    for sub in sorted(src_root.iterdir()):
        if not sub.is_dir():
            continue
        files = sorted(p for p in sub.iterdir() if p.suffix.lower() in (".exr", ".png"))
        out_dir = dest / sub.name
        out_dir.mkdir(parents=True, exist_ok=True)
        for i, p in enumerate(files, start=1):
            if sub.name == "depth_raw":
                name = p.name  # frame_00000.exr のまま
            else:
                name = f"{sub.name}.{i:05d}{p.suffix.lower()}"
            target = out_dir / name
            if keep_comfy_copy:
                shutil.copy2(p, target)
            else:
                shutil.move(str(p), str(target))
        counts[sub.name] = len(files)
    if not keep_comfy_copy:
        shutil.rmtree(src_root, ignore_errors=True)
    return counts


def normalize_depth_range(shot_dir, low_pct=2.0, high_pct=98.0):
    """
    深度 (16bit PNG) をショット全体の low_pct〜high_pct パーセンタイルで 0..1 に引き伸ばす。
    TypeGPU の作例 (disparity-range.ts) は各フレームの 2%〜98% を範囲にし、時間方向にゆっくり追従させる。
    動画はショット全体で 1 つの範囲にすれば追従を完全に止めた状態と同じで、ちらつかない。
    VideoDepthAnythingOutput の gray はショット全体の min/max 正規化なので、外れ値があると幅が圧縮される。
    """
    import cv2
    import numpy as np

    shot_dir = Path(shot_dir)
    mpath = shot_dir / "manifest.json"
    manifest = json.loads(mpath.read_text(encoding="utf-8")) if mpath.exists() else {}
    depth_info = manifest.get("depth") or {}
    if depth_info.get("range_applied"):
        log("深度の範囲正規化は適用済みです")
        return
    files = sorted((shot_dir / "depth").glob("depth.*.png"))
    if not files:
        log("深度 PNG がないため範囲正規化をスキップ")
        return
    # 全フレームから間引いて分布を取る (メモリを抑える)
    step = max(1, len(files) // 48)
    samples = []
    for p in files[::step]:
        a = cv2.imread(str(p), cv2.IMREAD_UNCHANGED)
        if a.ndim == 3:
            a = a[:, :, 0]
        samples.append(a[::4, ::4].astype(np.float32).ravel())
    allv = np.concatenate(samples)
    lo, hi = np.percentile(allv, [low_pct, high_pct])
    hi = max(hi, lo + 1.0)
    for p in files:
        a = cv2.imread(str(p), cv2.IMREAD_UNCHANGED).astype(np.float32)
        a = np.clip((a - lo) / (hi - lo), 0.0, 1.0)
        cv2.imwrite(str(p), (a * 65535.0 + 0.5).astype(np.uint16))
    depth_info["range_applied"] = True
    depth_info["normalization"] = f"shot-global {low_pct:g}-{high_pct:g} percentile (TypeGPU disparity-range 相当)"
    depth_info["range_source_u16"] = [float(lo), float(hi)]
    manifest["depth"] = depth_info
    mpath.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    log(f"深度を {low_pct:g}%〜{high_pct:g}% の範囲で正規化しました ({len(files)} フレーム, "
        f"元の範囲 {lo / 65535:.3f}〜{hi / 65535:.3f})")


def main():
    ap = argparse.ArgumentParser(description="法線・深度パスを ComfyUI で生成して EXR 連番にする")
    ap.add_argument("--video", help="入力動画 (mp4/mov/mkv/webm) または静止画 (png/jpg/webp)")
    ap.add_argument("--shot", help="ショット名 (cache/<shot>/ に保存)")
    ap.add_argument("--cache", default=str(DEFAULT_CACHE), help="キャッシュのルート")
    ap.add_argument("--server", default=DEFAULT_SERVER)
    ap.add_argument("--comfy-output", default=DEFAULT_COMFY_OUTPUT, help="ComfyUI の output フォルダ")
    ap.add_argument("--frame-cap", type=int, default=0, help="読み込む最大フレーム数 (0 = 全部)")
    ap.add_argument("--skip", type=int, default=0, help="先頭からスキップするフレーム数")
    ap.add_argument("--every-nth", type=int, default=1)
    ap.add_argument("--force-rate", type=float, default=0, help="フレームレート変換 (0 = 元のまま)")
    ap.add_argument("--normal-model", default=NORMAL_MODEL)
    ap.add_argument("--depth-model", default=DEPTH_MODEL)
    ap.add_argument("--moge-level", type=int, default=9, help="MoGe 解像度レベル 0..9")
    ap.add_argument("--batch", type=int, default=4, help="MoGe のバッチサイズ")
    ap.add_argument("--vda-input-size", type=int, default=518)
    ap.add_argument("--vda-max-res", type=int, default=2048, help="深度推定の最長辺上限 (元解像度に合わせる)")
    ap.add_argument("--format", choices=["png16", "exr"], default="png16", help="パスの保存形式")
    ap.add_argument("--no-normal", action="store_true")
    ap.add_argument("--no-depth", action="store_true")
    ap.add_argument("--depth-raw", action="store_true", help="生の逆深度 (Z チャンネル EXR) も保存")
    ap.add_argument("--keep-comfy-copy", action="store_true", help="ComfyUI/output 側の生成物を残す")
    ap.add_argument("--dry-run", action="store_true", help="ワークフロー JSON を表示して終了")
    ap.add_argument("--no-range", action="store_true", help="深度の 2%%〜98%% 範囲正規化をしない")
    ap.add_argument("--renormalize", metavar="SHOT_DIR", help="既存キャッシュの深度に範囲正規化だけ掛けて終了")
    ap.add_argument("--remote", action="store_true",
                    help="別の PC の深度サーバー (depth_server.py) で深度だけ作る。--server はそのサーバー")
    ap.add_argument("--temporal", type=float, default=0.9,
                    help="深度の時間方向のならし (0 で無し。--remote のとき)")
    ap.add_argument("--remote-long-side", type=int, default=960,
                    help="--remote のとき、深度の長辺 (推定は 518px で行うので、これより大きくしても細かくはならない)")
    a = ap.parse_args()

    if a.renormalize:
        normalize_depth_range(a.renormalize)
        return
    if not a.video or not a.shot:
        ap.error("--video と --shot が必要です")

    video = Path(a.video).resolve()
    if not video.exists():
        raise SystemExit(f"動画が見つかりません: {video}")
    a.video = str(video)
    if a.remote:
        if is_image(a.video):
            raise SystemExit("--remote は動画だけに対応しています")
        run_remote(a)
        return
    if a.no_normal and a.no_depth:
        raise SystemExit("--no-normal と --no-depth を同時には指定できません")

    shot = re.sub(r"[^\w\-]+", "_", a.shot)
    job = f"relight_{shot}_{uuid.uuid4().hex[:8]}"
    graph = build_workflow(a, job)

    if a.dry_run:
        print(json.dumps(graph, ensure_ascii=False, indent=2))
        return

    dest = Path(a.cache) / shot
    dest.mkdir(parents=True, exist_ok=True)

    def write_status(state, message=""):
        """プラグインが進捗・完了・失敗を知るための状態ファイル"""
        (dest / "status.json").write_text(
            json.dumps({"state": state, "message": message, "time": time.strftime("%Y-%m-%dT%H:%M:%S"),
                        "pid": os.getpid()},
                       ensure_ascii=False), encoding="utf-8")

    write_status("running", "ComfyUI へ接続中")
    try:
        try:
            api(a.server, "/system_stats", timeout=5)
        except Exception as e:
            raise SystemExit(f"ComfyUI に接続できません ({a.server}): {e}")

        log(f"ジョブ投入: {job}")
        write_status("running", "法線・深度を推定中")
        t0 = time.time()
        pid = submit(a.server, graph)
        entry = wait(a.server, pid)
        status = entry.get("status", {})
        if status.get("status_str") == "error":
            msgs = status.get("messages", [])
            raise SystemExit("ComfyUI 側で失敗しました:\n" + json.dumps(msgs, ensure_ascii=False, indent=2)[:4000])
        elapsed = time.time() - t0
        log(f"生成完了 ({elapsed:.1f}s)。ファイルを回収します")
        write_status("running", "ファイルを回収中")
        counts = collect(a.comfy_output, job, dest, a.keep_comfy_copy)
    except SystemExit as e:
        write_status("error", str(e))
        raise
    except Exception as e:
        write_status("error", f"{type(e).__name__}: {e}")
        raise

    manifest = {
        "shot": shot,
        "source": a.video,
        "source_id": file_sha1(a.video),
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "server": a.server,
        "frames": counts,
        "frame_cap": a.frame_cap,
        "skip": a.skip,
        "every_nth": a.every_nth,
        "force_rate": a.force_rate,
        "normal": None if a.no_normal else {
            "model": a.normal_model, "convention": "opengl", "encoding": "0..1 = (n+1)/2",
            "resolution_level": a.moge_level, "file": "normal/normal.%05d.exr",
        },
        "depth": None if a.no_depth else {
            "model": a.depth_model, "near": "white", "normalization": "shot-global min/max",
            "input_size": a.vda_input_size, "max_res": a.vda_max_res, "file": "depth/depth.%05d.exr",
            "raw": "depth_raw/frame_%05d.exr (Z, inverse depth)" if a.depth_raw else None,
        },
        "elapsed_sec": round(elapsed, 1),
    }
    ext = ".exr" if a.format == "exr" else ".png"
    manifest["format"] = a.format
    if manifest["normal"]:
        manifest["normal"]["file"] = "normal/normal.%05d" + ext
    if manifest["depth"]:
        manifest["depth"]["file"] = "depth/depth.%05d" + ext
    (dest / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    if a.format != "exr" and not a.no_depth and not a.no_range:
        write_status("running", "深度の範囲を正規化中")
        normalize_depth_range(dest)
    write_status("done", f"frames={counts}")
    log(f"完了: {dest}")
    for k, v in counts.items():
        log(f"  {k}: {v} フレーム")
    if a.format == "exr":
        log("AE では各フォルダの先頭 EXR を「EXR シーケンス」として読み込み、Interpret Footage で Preserve RGB を有効にしてください")


if __name__ == "__main__":
    main()
