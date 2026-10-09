# Relight Anime — relighting plugin for After Effects

[日本語](README.md) ｜ How it is built: [English](docs/how-it-works.en.md) / [日本語](docs/how-it-works.ja.md)

An After Effects effect that injects light into finished AI-generated anime footage (plain RGB video).
When the effect is applied, AE renders the source frames and a depth program (`relight_depth.exe`, ONNX Runtime + CUDA, no Python or ComfyUI)
estimates depth for the whole clip and wires it into the comp. Inside AE, the C++ effect only does the lighting. Lights can be dragged on the footage.

The lighting model is a C++ port of the TypeGPU example "Monocular Light Injection"
(`apps/typegpu-docs/src/examples/image-processing/monocular-light-injection`, MIT, Software Mansion):

- surface direction from the depth gradient
- occlusion from the depth height field
- cast shadows by ray-marching depth
- up to three point lights, with a visible light bulb and glow

Additions for anime (Look = Anime is the default; Look = Cinematic with the anime values at 0 gives the original picture):

- keeps the original colours: tints the unlit side with a shadow colour and adds light where it reaches
- cel shading (steps and softness)
- rim light, per light (placing a light behind the character gives back light)
- line-art protection (main lines and eyes keep their original colours)
- depth volume (rounds characters that live-action depth models see as flat)
- six presets

## Layout

```
plugin/            AE effect (C++ / SmartFX / 32 bpc / multi-frame rendering)
  src/RelightCore.h     the lighting itself (shared by CPU and GPU; TypeGPU port + anime additions)
  src/RelightGPU.cu     GPU path (CUDA), running the same RelightCore.h functions
  src/RelightAE.cpp     AE glue, CPU path, parameters, presets, light gizmos
  src/RelightAuto.cpp   automatic depth generation and wiring
  src/RelightHelper.cpp helper AEGP whose idle hook drives the automatic generation
  CMakeLists.txt / build.ps1
depth/
  relight_depth.cpp     depth estimation (Video Depth Anything Small as ONNX; CUDA / DirectML / CPU)
  build.ps1
models/            ONNX models (not in git; made by tools/export_vda_onnx.py --split)
third_party/onnxruntime/include   ONNX Runtime 1.23.2 C API headers
tools/
  export_vda_onnx.py   exports Video Depth Anything Small as encoder + head ONNX
  gen_passes.py        sends video to a remote depth server (--remote), or the older ComfyUI path
  depth_server/        depth server for Depth Engine = Remote (runs on another machine)
  harness/             tools that run the plugin's lighting code without AE (relight_cli, flicker metrics)
  *.jsx                AE scripts used for testing
docs/
  how-it-works.en.md / how-it-works.ja.md   how it is built
  設計.md                                     detailed design notes (Japanese)
```

## Requirements

- Windows 11, After Effects 2025 or later (developed on 2026 / 26.5)
- Visual Studio 2022 Build Tools (C++ workload), Ninja, CMake (`pip install cmake` works)
- **After Effects SDK** from https://developer.adobe.com/after-effects/ (pass its location with `build.ps1 -SdkRoot`)
- For local depth (default)
  - ONNX models: `python tools/export_vda_onnx.py --split` (weights `video_depth_anything_vits.pth`, Apache-2.0)
  - CUDA build of ONNX Runtime 1.23 and the CUDA 12 / cuDNN 9 DLLs; their locations go to `ort` and `dll_dirs` in the settings file
  - If CUDA fails, the ONNX Runtime bundled with AE (DirectML) is used automatically
- Optional: a Linux GPU machine for the remote depth server ([tools/depth_server/README.md](tools/depth_server/README.md))

## Build and install

```powershell
.\depth\build.ps1
.\plugin\build.ps1 -SdkRoot C:\dev\AE_SDK\AfterEffectsSDK_26.5_win -Install
# with a remote depth server:
.\plugin\build.ps1 -SdkRoot <SDK> -Install -RemoteServer https://<host>:8445
```

`-Install` copies `RelightAnime.aex` (effect) and `RelightAnimeHelper.aex` (helper AEGP) to
`C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\SRLM\` (administrator rights needed; close AE first).
The build also adds missing keys to `%APPDATA%\SRLM\RelightAnime.ini` (depth program, models, ONNX Runtime, CUDA DLLs, cache folder, remote server).

## Use in AE

1. Apply "SRLM > Relight Anime" to a video or image footage layer
2. AE renders the source frames and the depth program estimates depth. When done, a hidden layer "Relight depth" is added and wired
   (about 10 seconds for an 8-second clip on an RTX 5090; the cache in `cache/` makes the second time instant;
   progress is in `cache/relight_auto.log` and `cache/<source>/status.json`)
3. Select the effect: the light's cross appears in the comp viewer. Drag it to move the light, and use Light Height for front / back.
   Each light shows a ring in its colour with its number and height (dashed when behind objects) and a dotted range circle
4. Use Preset at the top to change the mood (values are overwritten and the menu returns to Select...)

To place a light behind the character (back light), set Light Height negative (e.g. −0.25): the bulb is hidden by the character,
only the background around them glows, the front of the character takes the shadow colour and the outline gets rim light.

"Clean Cache..." in Auto lists depth caches not used by the current project and deletes them after confirmation.

## Parameters

| Group | Parameter | Meaning |
|---|---|---|
| (top) | Preset | Anime Standard (default) / Soft Daylight / Sunset Backlight / Night Neon / Cel Hard Light / Cinematic (TypeGPU) |
| Auto | Auto Passes / Generate Passes | Automatic depth on apply; the button regenerates |
| | Clean Cache... | Delete depth caches not used by this project (after confirmation) |
| | Depth Engine | Where depth is made when Generate Passes is pressed: Local GPU (this PC, small model, ~10 s for 8 s of video) / Remote Fast (remote depth server, small model, ~45 s) / Remote Quality (large model, ~2 min 20 s, about one third of the flicker). Remote does not use this PC's GPU; if unreachable, depth is made locally |
| Light | Light Position | Light position (draggable) |
| | Light Height | Front / back; 0 is the nearest surface, negative is behind objects (back light) |
| | Light Color / Intensity | Colour and strength |
| | Range | Reach of the light (1 = default; higher reaches farther, lower stays near the light). Shown as a dotted circle in the comp viewer |
| | Show Light | Draw this light's bulb and glow |
| | Rim (group) | This light's rim light; see the Rim table below |
| Light 2 / Light 3 | Enable / Position / Height / Color / Intensity / Range / Show Light / Rim | Second and third lights (disabled by default; Show Light off by default). They may be placed off screen as fill lights |
| Scene | Look | Anime (keeps original colours) / Cinematic (original: darkens everything, filmic tone map) |
| | Ambient / Ambient Color | Brightness and tint of the original picture; lower makes the light stand out |
| | Relief | Strength of relief from depth |
| | Specular | Specular highlights |
| | Shadow | Cast shadow strength |
| | Occlusion | Darkness of crevices |
| Anime Style | Form Shading | Share of 3D shading from surface direction. 0 = compositing style (smooth on-screen gradient around the light, stable over time), 1 = 3D (follows shape, flickers more). Default 0.25 |
| | Light Spread | Width of the compositing-style gradient (screen height = 1; default 0.7) |
| | Shadow Color / Shadow Tint | Colour and strength put on the unlit side |
| | Edge Dither | Near outlines only, smooths the effect along the outline and jitters the sampling to break up steps |
| | Shade Smoothness | Smooths shading over large areas without crossing outlines (even tone with soft edges, like a composited shadow) |
| | Cel Shading / Cel Steps / Cel Softness | Cel strength, number of steps, softness of step edges |
| | Line Protect / Line Threshold | How strongly line art keeps its colour, and how dark counts as a line (check with Output = Line Mask) |
| | Depth Volume | Bulges flat characters forward |
| Passes | Depth Layer / Depth Near Is | Depth layer (near = white by default) |
| | Depth Stabilize | Temporal median of depth over neighbouring frames (Off / 1 Frame / 2 Frames, default 2) |
| | Snap Depth To Lines | Aligns depth edges to the footage's line art (default on) |
| | Normal Source / Normal Layer | Surface direction from depth, or from a normal layer |
| | Pass Encoding | Linear (PNG or EXR with Preserve RGB) / sRGB (decode) (when AE converted an EXR) |
| | Mask Layer / Mask Source | Mask that limits the effect |
| | Input Is Linear | For linear working spaces |
| | Use GPU | Render on the GPU (CUDA), default on; only when the project uses Mercury GPU Acceleration (CUDA) |
| Output | Relit / Depth / Normals / Shadow & Occlusion / Line Mask | Output type |
| | Mix | Amount |

Rim (inside each Light; set per light)

| Parameter | Meaning |
|---|---|
| Use Rim | Whether this light makes rim light (off = it only lights surfaces) |
| Rim Light / Rim Width | Strength and width of the outline light (pixels at 1080p) |
| Rim Softness | 0 = crisp band; higher fades the inner side |
| Rim Smoothing | Smooths the outline before building the band (median filter on depth), removing depth noise and stair steps for clean anime-like lines. Default 0.5 |
| Rim Placement | Auto (light in front: light side only, subdued; light behind: whole outline) / Light Side / All Around |
| Rim Color | Rim colour (multiplies the light colour; white keeps the light colour) |
| Rim Spread | How far the rim wraps from the light-facing outline (0 = light-facing side only, 1 = whole outline; default 0.35) |
| Rim Reach | How far from the light on screen the rim reaches (screen height = 1, 3 = unlimited; default 0.9) |

## Depth Engine = Remote (depth server on another machine)

- A depth server on another machine (e.g. a Linux GPU box; `tools/depth_server/depth_server.py`, no ComfyUI) takes a video with `POST /depth?model=vits|vitl`,
  runs Video Depth Anything, normalizes over the shot (2–98 %) and smooths over time, and returns 16-bit depth (npz).
- systemd socket activation starts it on a request; it exits after 5 idle minutes. It listens on `127.0.0.1` only; expose it with e.g. Tailscale Serve.
- Setup and systemd unit templates: [tools/depth_server/README.md](tools/depth_server/README.md).
- The target is `remote_server` in `%APPDATA%\SRLM\RelightAnime.ini` (or `plugin\build.ps1 -RemoteServer <URL>`). Empty means depth is made locally.

## Status

- Lighting verified with the out-of-AE harness (same code as the plugin) on stills and video frames
- Automatic generation (render → infer → import → wire) verified in AE 2026 (via the helper AEGP)
- Speed (1376×768): GPU (RTX 5090, CUDA) about 1–3 ms per frame; CPU (24 threads) about 50 ms. GPU and CPU differ by at most one 8-bit step
- To render on the GPU, set File > Project Settings > Video Rendering and Effects to Mercury GPU Acceleration (CUDA); otherwise the CPU path is used

## Licences

The lighting model is ported from TypeGPU (MIT, Software Mansion). Video Depth Anything Small is Apache-2.0; **Large is CC-BY-NC-4.0 (non-commercial)** —
Remote Quality uses Large. ONNX Runtime headers are MIT. The After Effects SDK is not included. See [docs/how-it-works.en.md](docs/how-it-works.en.md#10-licences).
