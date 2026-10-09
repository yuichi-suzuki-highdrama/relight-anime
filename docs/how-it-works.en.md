# How Relight Anime is built

[日本語](how-it-works.ja.md) ｜ [README](../README.en.md) ｜ Detailed design notes (Japanese): [設計.md](設計.md)

Relight Anime is an After Effects effect that adds light and shadow, after the fact, to finished AI-generated anime footage (plain RGB video).
This document explains how it is put together and how to build it from scratch.

---

## 1. Overview

```
Apply Relight Anime to a footage layer
  │
  ├─ Make depth (once; cached in cache/)
  │    AE renders the source frames → relight_depth.exe (ONNX Runtime + CUDA) estimates depth
  │    or the video is sent to a depth server on another machine (Depth Engine = Remote)
  │    → 16-bit depth PNGs are imported and wired as a hidden layer "Relight depth"
  │
  └─ Light every frame
       From depth: surface direction, occlusion, cast shadows; then each light is applied and
       light/shadow is added to the original picture.
       CPU and GPU (CUDA) paths share the same lighting code (RelightCore.h).
```

Depth estimation runs outside AE's render call because AE requests frames in any order and in parallel,
while video depth estimation must process neighbouring frames together to be temporally stable.

## 2. Making depth

### 2.1 Model

- **Video Depth Anything Small** (Apache-2.0): a video depth model that looks at several frames at once, so it is much steadier over time than per-image models.
- `tools/export_vda_onnx.py --split` exports it as two ONNX files: the **encoder** (DINOv2) and the **temporal head**.
  - The encoder is per-frame, so frames go through 4 at a time (32 at once used up 32 GB of VRAM).
  - The head runs per 32-frame window.

### 2.2 relight_depth.exe (`depth/relight_depth.cpp`)

A standalone C++ program (no Python, no ComfyUI). The ONNX Runtime DLL is loaded at run time.

1. Read the frames AE rendered (`frames.bin`, short side ≥ 518 px)
2. Infer with the same windowing as the reference implementation
   - 32-frame windows advanced by 22 frames; the first 10 slots hold key frames of the previous window (their features are reused)
   - Each window is scale/shift-aligned to two key frames of the previous window by least squares; the 8 overlapping frames are cross-faded
3. **Temporal smoothing** (anti-flicker, §5)
4. Normalize with the **shot-global 2–98 % percentiles** to 0..1 (near = 1) and write 16-bit grey PNGs
5. If CUDA fails, retry with the ONNX Runtime bundled with AE (DirectML)

### 2.3 Making depth on another machine (Depth Engine = Remote)

`tools/depth_server/depth_server.py` is a tiny HTTP server that only makes depth (no ComfyUI).
It takes a video with `POST /depth`, runs the official Video Depth Anything code, does normalization and temporal smoothing on the GPU, and returns the result.
It is socket-activated by systemd: it starts on a request and exits after 5 idle minutes.
The effect's **Depth Engine** chooses Local GPU / Remote Fast (small model) / Remote Quality (large model);
if the server cannot be reached, depth is made locally instead. Setup: [tools/depth_server/README.md](../tools/depth_server/README.md).

## 3. Lighting (port of the TypeGPU example)

The lighting model is a C++ port of the TypeGPU example "Monocular Light Injection" (Software Mansion, MIT). Constants and formulas follow the original.

| Step | What it does |
|---|---|
| Surface direction | Depth gradient (7 texels), biased toward the smaller one-sided difference; noise is subtracted and the slope is capped with tanh |
| Occlusion | Counts depth samples that stick out in front, at 16 points on radii 3 / 9 texels |
| Cast shadow | 32-step ray march toward the light |
| Lighting | colour × ambient × occlusion + light colour × wrapped Lambert × falloff × shadow × intensity + specular |
| Light bulb | A sphere of radius 0.05, occluded by nearer objects, with halo and veil glow |
| Finish | Luminance-based filmic tone map |

The original works on square video with a 448×448 depth map; this port extends it to any aspect ratio:
`u = 0.5 + (px − W/2) / H`, `v = py / H`, and one texel is `H / 448` pixels.
Depth, gradients, occlusion, shadows and light coverage are computed on a "448 cells per screen height" grid and interpolated per pixel.

## 4. Additions for anime

The original targets camera footage: it darkens everything and applies a filmic curve, which dulls vivid anime colours.
With Look = Anime (default) the effect works as below. Look = Cinematic gives the original formula.

| Feature | How |
|---|---|
| Look = Anime | Keeps the original colours; tints the side the light does not reach with a shadow colour and adds light where it reaches; only very bright values are compressed, keeping hue |
| Compositing-style shading | A smooth on-screen gradient by distance from the light (like a compositing "para" light). It does not use fine surface direction, so it does not flicker. Mixed with 3D shading by Form Shading |
| Cel shading | Light coverage is quantized into steps with softened edges |
| Line-art protection | Thin areas darker than their surroundings are treated as lines and restored to their original colour; large dark areas such as black hair are excluded |
| Depth volume | Depth models trained on live action tend to see anime characters as flat, so depth is bulged forward by distance from the silhouette |
| Snap depth to lines | When upscaling depth (short side 518 px) to the footage size, samples whose footage colour is close get more weight (joint bilateral upsampling), so depth edges follow the line art |
| Three lights | Each light has position, height, colour, intensity, Range, Show Light and its own rim settings |
| Rim light | §4.1 |

### 4.1 Rim light

It uses the classic compositing trick for rim light and translucency, **shift the matte toward the light and subtract**, but with depth:

1. From each pixel, look at depth at 6 points within the rim width toward the light; the fraction that lands clearly behind (outside the object) is the band
2. For back light, the lit part of the outline is limited by the angle between the outline normal and the light (Rim Spread), and outlines far from the light on screen get weaker (Rim Reach)
3. The band is blurred slightly, and pixels that had no band before blurring are cleared (no glow outside the silhouette)
4. No band in the gap where the depth edge sits outside the drawn outline (light never spills past the character's line)
5. **Rim Smoothing**: before building the band, a median filter is applied to depth. It keeps steps but removes bumps smaller than the radius (depth noise, coarse-resolution stair steps), so the band's lines become clean. Unlike a blur it does not weaken depth differences and does not exaggerate them where several layers overlap
6. Where the rim lands, line-art protection keeps only the core of the line (protecting the soft edge of the outline chipped the band into a jagged dotted line)

An earlier version searched for the outline pixel by pixel and measured the distance; a tiny wobble in that search made the band width step and break up like beads.

## 5. Keeping video from flickering

Depth estimates wobble slightly every frame even when the character is still. Lighting turns that into brightness changes that look like a candle flame,
especially with a low light behind the character, where the "lit side / back side" decision and cel steps amplify it.

| Measure | What it does |
|---|---|
| Shot-global normalization | The original normalizes per frame and follows over time; for video one range is used for the whole shot |
| Temporal smoothing when making depth | Only where the original picture's colour does not change, keep 0.9 of the previous (and next) smoothed depth; average forward and backward passes. Moving areas change colour and are not mixed, so there is no ghosting |
| Prefer compositing-style shading | The share of the noise-prone 3D shading is limited by Form Shading (default 0.25) |
| Depth Stabilize | Median of depth over ±2 frames; removes values that jump out for 1–2 frames |

Flicker is measured after warping the previous frame onto the current one with optical flow (DIS) (`tools/harness/flow_flicker.py`);
comparing the same pixel would count the character's motion as flicker.
With a low light behind the character, the worst-case flicker went from 0.014 to 0.009 with smoothing, and to 0.003 with the large model (Remote Quality).

## 6. Speed (CPU and GPU)

- The lighting code lives in `plugin/src/RelightCore.h`; every function compiles for both CPU and CUDA (`__host__ __device__`).
- The CPU path (`RelightAE.cpp`, rows on worker threads) and the GPU path (`RelightGPU.cu`, one pixel / cell per thread) each own the overall procedure and call the same functions. They differ by at most one 8-bit step (`tools/harness/compare_gpu.py`).
- For AE's GPU render (`PF_Cmd_SMART_RENDER_GPU`) input, output and depth worlds arrive in GPU memory (BGRA float) and are used directly. Work buffers are reused, not allocated per frame.
- At 1376×768: about 1–3 ms on the GPU, about 50 ms on the CPU (RTX 5090 / 24 threads).

## 7. Notes on integrating with AE

| Topic | Approach |
|---|---|
| Frame order | AE requests frames in any order and in parallel; depth estimation runs outside rendering, in a separate process |
| Automatic depth | An idle hook registered from the effect was never called, and calling AEGP from a timer crashed AE; a helper AEGP (`RelightAnimeHelper.aex`) drives the work from its idle hook |
| Cache | Keyed by the source file name, path, size and modification time; jobs for the same source are shared |
| Pass format | 16-bit PNG (values arrive unchanged without colour management); EXR needs Preserve RGB |
| Input | Premultiplied; un-premultiplied for the maths, mixed with the original, then premultiplied again |
| Adding / reordering parameters | Parameter IDs are fixed so saved projects keep their values (e.g. when rim settings moved into each light, Light 1 reused the old IDs) |
| GPU | GPU rendering is offered only on CUDA; other frameworks (DirectX, etc.) use the CPU path |
| Comp viewer | Draws, per light, a ring in the light colour, its number and height, and a dotted range circle; dragging uses AE's standard 2D point handle |

## 8. Verification tools (`tools/harness/`)

| Tool | Purpose |
|---|---|
| `relight_cli` (`run_cli.py`) | Compiles the plugin code as-is and renders one frame without AE (CPU / GPU) |
| `compare_gpu.py` | Difference and timing between CPU and GPU paths |
| `flow_flicker.py` | Motion-compensated flicker (optical flow) |
| `rim_view.py` / `rim_flicker.py` | Zoom into the rim only / frame-to-frame rim jitter |
| `depth_ab.py` | Compare shadows produced by two depth sequences |
| `compare_presets.py` | Side-by-side comparison of presets |

## 9. Building from scratch

Requirements: Windows 11, After Effects 2025 or later, Visual Studio 2022 Build Tools (C++), CMake, Ninja, CUDA Toolkit 12.x, Python (for model export), After Effects SDK.

1. Download the **AE SDK** from https://developer.adobe.com/after-effects/ and extract it (it is not in this repository)
2. Make the **ONNX models**: `python tools/export_vda_onnx.py --split` (written to `models/`; weights: `video_depth_anything_vits.pth`)
3. Build the **depth program**: `.\depth\build.ps1`
4. Build and install the **plugin** (administrator):
   `.\plugin\build.ps1 -SdkRoot <SDK path> -Install`
   - Writes the locations of the depth program, models, ONNX Runtime and CUDA DLLs to `%APPDATA%\SRLM\RelightAnime.ini` (only missing keys are added)
   - The CUDA ONNX Runtime and cuDNN DLL locations differ per machine; check `ort` and `dll_dirs` in that file
5. (Optional) To make depth on another machine, set up the server as in [tools/depth_server/README.md](../tools/depth_server/README.md) and build with `-RemoteServer <URL>`
6. In AE, apply "SRLM > Relight Anime" to a footage layer. Depth is wired automatically when it is ready

## 10. Licences

| Component | Terms |
|---|---|
| This project | MIT ([LICENSE](../LICENSE)) |
| TypeGPU (origin of the lighting model) | MIT; keep the copyright notice (shown in the effect's About) |
| Video Depth Anything Small | Apache-2.0 |
| Video Depth Anything Large (used by Remote Quality) | **CC-BY-NC-4.0 (non-commercial)**; use Small for commercial work |
| ONNX Runtime (headers included) | MIT |
| After Effects SDK | Adobe SDK licence (not included in this repository) |

## 11. Known limitations

- Shadows baked into the original picture remain (the effect darkens and adds light on top)
- Background depth may be treated as a distant wall and receive large cast shadows (adjust Shadow, or limit with Mask Layer)
- Depth does not follow time-stretched / time-remapped layers. If a layer is duplicated after its depth exists and moved in time, the copy still points to the original depth layer and goes out of sync
- Up to three lights
