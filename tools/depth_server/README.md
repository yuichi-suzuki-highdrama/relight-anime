# 深度サーバー / Depth server

Relight Anime の Depth Engine = Remote Fast / Remote Quality が使う、深度だけを作る小さな HTTP サーバー。ComfyUI は要らない。
A tiny HTTP server that only produces depth for Relight Anime's Depth Engine = Remote Fast / Remote Quality. No ComfyUI needed.

```
POST /depth?model=vits|vitl&temporal=0.9&long_side=960   (body = the video file)
  → 200 application/octet-stream: npz
      depth          uint16 [N, H, W]   near = 65535, shot-global 2–98 % normalized, temporally smoothed
      fps, range_source, model, temporal_keep
GET  /health → {"ok": true, "busy": false, ...}
```

## 必要なもの / Requirements

- Linux + NVIDIA GPU (CUDA)、Python 3.10 以降、PyTorch (CUDA)
- `opencv-python-headless numpy einops easydict matplotlib imageio`
- Video Depth Anything の公式コード（`video_depth_anything/` フォルダ）
  official code: https://github.com/DepthAnything/Video-Depth-Anything (Apache-2.0)
- 重み / weights（Hugging Face）
  - `video_depth_anything_vits.pth` — depth-anything/Video-Depth-Anything-Small (Apache-2.0)
  - `video_depth_anything_vitl.pth` — depth-anything/Video-Depth-Anything-Large (**CC-BY-NC-4.0, non-commercial**)

## 置き方 / Layout

```
<ROOT>/                    例 / e.g. ~/relight-depth
  server/
    depth_server.py        この repo の tools/depth_server/depth_server.py
    video_depth_anything/  公式のコードをコピー / copied from the official repo
  models/
    video_depth_anything_vits.pth
    video_depth_anything_vitl.pth
  venv/                    Python 環境 / Python environment
```

試しに起動する / quick test:

```bash
cd <ROOT>/server
RELIGHT_MODEL_DIR=<ROOT>/models ../venv/bin/python depth_server.py --port 8191
curl http://127.0.0.1:8191/health
```

## systemd（依頼が来たときだけ起動する / start on demand）

`relight-depth.socket` と `relight-depth.service` を `~/.config/systemd/user/` に置き、`<ROOT>` を書き換える。
Put the two unit files in `~/.config/systemd/user/` and replace `<ROOT>`.

```bash
systemctl --user daemon-reload
systemctl --user enable --now relight-depth.socket
loginctl enable-linger $USER     # ログインしていなくても動かす / keep running without a login session
```

サーバーは `127.0.0.1:8190` だけで待ち受ける。ほかの PC から使うときは、Tailscale Serve などで必要な範囲だけに公開する。
The server listens on `127.0.0.1:8190` only. Expose it to the AE machine with e.g. Tailscale Serve:

```bash
sudo tailscale serve --bg --https=8445 http://127.0.0.1:8190
```

5 分（`RELIGHT_IDLE_EXIT`）依頼が無ければ自分で終わり、次の依頼でまた起動する。
It exits after 5 minutes (`RELIGHT_IDLE_EXIT`) without requests and is started again by the socket.

## AE 側の設定 / AE side

`%APPDATA%\SRLM\RelightAnime.ini` の `remote_server` に URL を書く（`plugin\build.ps1 -RemoteServer https://<host>:8445` でも書ける）。
Set `remote_server` in `%APPDATA%\SRLM\RelightAnime.ini` (or run `plugin\build.ps1 -RemoteServer https://<host>:8445`).
