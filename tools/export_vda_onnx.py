#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
export_vda_onnx.py - Video Depth Anything (Small, Apache-2.0) を ONNX に書き出す (CPU で実行)

  python tools/export_vda_onnx.py
  python tools/export_vda_onnx.py --check   # 書き出した ONNX を onnxruntime (CPU) で PyTorch と比較

入力  frames: [1, 32, 3, H, W]  (H, W は 14 の倍数、ImageNet の平均・分散で正規化済み)
出力  depth:  [1, 32, H, W]     (相対的な逆深度。近いほど大きい)

モデルのコードは ComfyUI-Video-Depth-Anything に同梱のもの、重みは ComfyUI の models から読む。
"""
import argparse
import sys
import time
from pathlib import Path

import numpy as np
import torch

NODE_DIR = Path(r"C:\ComfyUI_windows_portable\ComfyUI\custom_nodes\ComfyUI-Video-Depth-Anything")
WEIGHTS = Path(r"C:\ComfyUI_windows_portable\ComfyUI\models\videodepthanything\video_depth_anything_vits.pth")
OUT = Path(__file__).resolve().parent.parent / "models" / "vda_small_t32.onnx"
T = 32


def _shim_easydict():
    """easydict が無ければ、属性でも引ける dict の代用品を入れる (dpt_temporal.py が設定値の受け渡しに使うだけ)"""
    try:
        import easydict  # noqa: F401
        return
    except ImportError:
        pass
    import types

    class EasyDict(dict):
        def __init__(self, d=None, **kw):
            super().__init__()
            for k, v in dict(d or {}, **kw).items():
                self[k] = v

        def __getattr__(self, k):
            try:
                return self[k]
            except KeyError as e:
                raise AttributeError(k) from e

        def __setattr__(self, k, v):
            self[k] = v

        def __setitem__(self, k, v):
            if isinstance(v, dict) and not isinstance(v, EasyDict):
                v = EasyDict(v)
            super().__setitem__(k, v)

    mod = types.ModuleType("easydict")
    mod.EasyDict = EasyDict
    sys.modules["easydict"] = mod


def patch_for_export():
    """
    ONNX で入力サイズを可変にするための差し替え (ComfyUI 側のファイルは変更しない)。
    1. 時間方向の注意: baddbmm(empty(...), beta=0) は画面サイズ分の空テンサーが定数として焼き込まれる → bmm に
    2. DINOv2 の位置埋め込み補間: float()/int() でサイズが固定される → 出力サイズ指定の補間に
    3. DPT ヘッドの int(patch_h * 14): モジュール内の int を素通しにしてサイズを可変のまま残す
    """
    import torch.nn.functional as F
    from video_depth_anything.motion_module import attention as mm_attention
    from video_depth_anything import dinov2 as dino_mod
    from video_depth_anything import dpt_temporal

    def _attention(self, query, key, value, attention_mask=None):
        scores = torch.bmm(query, key.transpose(-1, -2)) * self.scale
        if attention_mask is not None:
            scores = scores + attention_mask
        probs = scores.softmax(dim=-1).to(value.dtype)
        return self.reshape_batch_dim_to_heads(torch.bmm(probs, value))

    mm_attention.CrossAttention._attention = _attention

    def interpolate_pos_encoding(self, x, w, h):
        previous_dtype = x.dtype
        pos_embed = self.pos_embed.float()
        class_pos_embed = pos_embed[:, 0]
        patch_pos_embed = pos_embed[:, 1:]
        dim = x.shape[-1]
        w0 = w // self.patch_size
        h0 = h // self.patch_size
        M = self._pe_grid   # 書き出し前に Python の整数として求めておいた格子の一辺 (37)
        patch_pos_embed = F.interpolate(
            patch_pos_embed.reshape(1, M, M, dim).permute(0, 3, 1, 2),
            size=(w0, h0), mode="bicubic", align_corners=False,
        )
        patch_pos_embed = patch_pos_embed.permute(0, 2, 3, 1).reshape(1, -1, dim)
        return torch.cat((class_pos_embed.unsqueeze(0), patch_pos_embed), dim=1).to(previous_dtype)

    for name in dir(dino_mod):
        cls = getattr(dino_mod, name)
        if isinstance(cls, type) and hasattr(cls, "interpolate_pos_encoding"):
            cls.interpolate_pos_encoding = interpolate_pos_encoding

    dpt_temporal.int = lambda v: v

    # 4. 時間方向モジュールの einops.rearrange は形を Python の整数で確定させるため、
    #    書き出し時の画面サイズが定数として残る → 同じ並べ替えを reshape / permute で書き直す
    from video_depth_anything.motion_module import motion_module as mm

    def rearrange(x, pattern, **kw):
        if pattern == "b c f h w -> (b f) c h w":
            return x.permute(0, 2, 1, 3, 4).flatten(0, 1)
        if pattern == "(b f) c h w -> b c f h w":
            f = kw["f"]
            return x.reshape(-1, f, x.shape[1], x.shape[2], x.shape[3]).permute(0, 2, 1, 3, 4)
        if pattern == "(b f) d c -> (b d) f c":
            f = kw["f"]
            return x.reshape(-1, f, x.shape[1], x.shape[2]).permute(0, 2, 1, 3).reshape(-1, f, x.shape[2])
        if pattern == "(b d) f c -> (b f) d c":
            d = kw["d"]
            return x.reshape(-1, d, x.shape[1], x.shape[2]).permute(0, 2, 1, 3).reshape(-1, d, x.shape[2])
        raise NotImplementedError(pattern)

    mm.rearrange = rearrange


def load_model(for_export=False):
    _shim_easydict()
    sys.path.insert(0, str(NODE_DIR))
    if for_export:
        patch_for_export()
    from video_depth_anything.video_depth import VideoDepthAnything  # noqa: E402
    model = VideoDepthAnything(encoder="vits", features=64, out_channels=[48, 96, 192, 384], metric=False)
    sd = torch.load(str(WEIGHTS), map_location="cpu", weights_only=True)
    model.load_state_dict(sd, strict=True)
    import math
    model.pretrained._pe_grid = math.isqrt(int(model.pretrained.pos_embed.shape[1]) - 1)
    return model.eval()


def export(model, h, w):
    """
    dynamo 方式 (torch.export) で書き出す。従来方式は x.shape を分解した値を定数として記録するため、
    入力サイズが書き出し時に固定されてしまう。縦横は 14 の倍数 (パッチ数) を記号のまま扱う。
    """
    from torch.export import Dim
    OUT.parent.mkdir(parents=True, exist_ok=True)
    x = torch.randn(1, T, 3, h, w)
    ph = Dim("ph", min=8, max=128)
    pw = Dim("pw", min=8, max=128)
    t0 = time.time()
    prog = torch.onnx.export(
        model, (x,), dynamo=True,
        input_names=["frames"], output_names=["depth"],
        dynamic_shapes={"x": {3: 14 * ph, 4: 14 * pw}},
        opset_version=18, optimize=True,
    )
    prog.save(str(OUT))
    print(f"書き出し: {OUT} ({OUT.stat().st_size / 1e6:.1f} MB, {time.time() - t0:.1f}s)")


def check(ref_model, sizes):
    """元の PyTorch 実装 (差し替えなし) と ONNX (CPU) の出力を比べる。入力は実画像に近い滑らかな乱数"""
    import onnxruntime as ort
    so = ort.SessionOptions()
    sess = ort.InferenceSession(str(OUT), so, providers=["CPUExecutionProvider"], disabled_optimizers=["ReshapeFusion"])
    g = torch.Generator().manual_seed(0)
    for (h, w) in sizes:
        base = torch.rand(1, 1, 3, h // 28, w // 28, generator=g)
        x = torch.nn.functional.interpolate(base[0], size=(h, w), mode="bilinear")[None].repeat(1, T, 1, 1, 1)
        x = x + 0.05 * torch.randn(1, T, 3, h, w, generator=g)
        x = (x - 0.45) / 0.225
        with torch.no_grad():
            ref = ref_model(x).numpy()
        t0 = time.time()
        out = sess.run(None, {"frames": x.numpy()})[0]
        dt = time.time() - t0
        rel = np.abs(out - ref).mean() / max(np.abs(ref).mean(), 1e-6)
        corr = np.corrcoef(out.ravel(), ref.ravel())[0, 1]
        print(f"  {h}x{w}: 出力 {out.shape}, 平均相対差 {rel:.2e}, 相関 {corr:.5f}, ORT(CPU) {dt:.1f}s")


ENC_OUT = OUT.parent / "vda_small_encoder.onnx"
HEAD_OUT = OUT.parent / "vda_small_head_t32.onnx"
LAYERS = [2, 5, 8, 11]   # vits の中間層


class Encoder(torch.nn.Module):
    """DINOv2 部分。1 フレームずつ独立なので、小分けにして通せる。出力は [N, 384, ph, pw] x 4"""
    def __init__(self, m):
        super().__init__()
        self.pretrained = m.pretrained

    def forward(self, x):
        feats = self.pretrained.get_intermediate_layers(x, LAYERS, reshape=True, return_class_token=False)
        return tuple(feats)


class Head(torch.nn.Module):
    """時間方向のヘッド。32 フレーム分の特徴をまとめて受け取り、深度 [1, 32, 14*ph, 14*pw] を返す"""
    def __init__(self, m):
        super().__init__()
        self.head = m.head

    def forward(self, f1, f2, f3, f4):
        ph, pw = f1.shape[2], f1.shape[3]
        feats = [(f.flatten(2).transpose(1, 2), None) for f in (f1, f2, f3, f4)]
        depth = self.head(feats, ph, pw, T)[0]
        depth = torch.nn.functional.relu(depth)
        return depth.squeeze(1).unsqueeze(0)


def export_split(model, h, w):
    """VRAM を抑えるため、エンコーダとヘッドを別の ONNX に書き出す"""
    from torch.export import Dim
    ph = Dim("ph", min=8, max=128)
    pw = Dim("pw", min=8, max=128)
    n = Dim("n", min=1, max=64)
    enc, head = Encoder(model).eval(), Head(model).eval()
    x = torch.randn(4, 3, h, w)
    t0 = time.time()
    prog = torch.onnx.export(enc, (x,), dynamo=True, input_names=["frames"],
                             output_names=["f1", "f2", "f3", "f4"],
                             dynamic_shapes={"x": {0: n, 2: 14 * ph, 3: 14 * pw}}, opset_version=18, optimize=True)
    prog.save(str(ENC_OUT))
    print(f"書き出し: {ENC_OUT} ({ENC_OUT.stat().st_size / 1e6:.1f} MB, {time.time() - t0:.1f}s)")
    with torch.no_grad():
        f = enc(torch.randn(T, 3, h, w))
    t0 = time.time()
    shp = {k: {2: ph, 3: pw} for k in ("f1", "f2", "f3", "f4")}
    prog = torch.onnx.export(head, tuple(f), dynamo=True, input_names=["f1", "f2", "f3", "f4"],
                             output_names=["depth"], dynamic_shapes=shp, opset_version=18, optimize=True)
    prog.save(str(HEAD_OUT))
    print(f"書き出し: {HEAD_OUT} ({HEAD_OUT.stat().st_size / 1e6:.1f} MB, {time.time() - t0:.1f}s)")


def check_split(ref_model, sizes, chunk=4):
    """エンコーダを chunk 枚ずつ通してからヘッドに渡した結果を、元の実装と比べる"""
    import onnxruntime as ort
    enc = ort.InferenceSession(str(ENC_OUT), providers=["CPUExecutionProvider"])
    head = ort.InferenceSession(str(HEAD_OUT), providers=["CPUExecutionProvider"])
    g = torch.Generator().manual_seed(0)
    for (h, w) in sizes:
        base = torch.rand(1, 1, 3, h // 28, w // 28, generator=g)
        x = torch.nn.functional.interpolate(base[0], size=(h, w), mode="bilinear")[None].repeat(1, T, 1, 1, 1)
        x = (x + 0.05 * torch.randn(1, T, 3, h, w, generator=g) - 0.45) / 0.225
        with torch.no_grad():
            ref = ref_model(x).numpy()
        t0 = time.time()
        parts = [enc.run(None, {"frames": x[0, i:i + chunk].numpy()}) for i in range(0, T, chunk)]
        feats = {f"f{k + 1}": np.concatenate([p[k] for p in parts], 0) for k in range(4)}
        out = head.run(None, feats)[0]
        dt = time.time() - t0
        rel = np.abs(out - ref).mean() / max(np.abs(ref).mean(), 1e-6)
        corr = np.corrcoef(out.ravel(), ref.ravel())[0, 1]
        print(f"  分割 {h}x{w}: 出力 {out.shape}, 平均相対差 {rel:.2e}, 相関 {corr:.5f}, ORT(CPU) {dt:.1f}s")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--split", action="store_true", help="エンコーダとヘッドに分けて書き出す (推奨)")
    ap.add_argument("--export-size", type=int, nargs=2, default=[518, 924], metavar=("H", "W"))
    a = ap.parse_args()
    torch.set_num_threads(8)
    if a.split:
        if not a.check:
            with torch.no_grad():
                export_split(load_model(for_export=True), *a.export_size)
            print("比較は --split --check を別に実行してください")
            return
        check_split(load_model(for_export=False), [(518, 938), (938, 518)])
        return
    if not a.check:
        # 差し替えはクラスに入るので、書き出しは別プロセスでも同じ。ここでは書き出し後に元実装を読み直さない
        exp_model = load_model(for_export=True)
        with torch.no_grad():
            export(exp_model, *a.export_size)
        print("比較は --check を別に実行してください (元実装と比べるため)")
        return
    ref_model = load_model(for_export=False)
    # 書き出し時と違うサイズでも動くか (入力サイズ可変の確認)
    check(ref_model, [(518, 924), (924, 518), (518, 518), (518, 784)])


if __name__ == "__main__":
    main()
