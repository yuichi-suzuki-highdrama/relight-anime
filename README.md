# Relight Anime - After Effects 再照明プラグイン

[English](README.en.md) ｜ 作り方: [日本語](docs/how-it-works.ja.md) / [English](docs/how-it-works.en.md) ｜ 詳しい設計: [docs/設計.md](docs/設計.md)

AI アニメ素材（完成済み RGB 動画）に、後から光を注入する AE プラグイン。
素材にエフェクトを適用すると、AE が素材のフレームを書き出し、深度推定プログラム（relight_depth.exe、
ONNX Runtime + CUDA、Python と ComfyUI は不要）が動画全体の深度を作ってコンポに結線する。
AE 内では C++ エフェクトが照明だけを計算する。光源は素材の上でドラッグして動かせる。

照明モデルは TypeGPU の作例「Monocular Light Injection」
（`apps/typegpu-docs/src/examples/image-processing/monocular-light-injection`、MIT、Software Mansion）を C++ に移植したもの。

- 深度の勾配から面の向きを作る
- 深度の高さ場から凹みを暗くする
- 深度をレイマーチして落ち影を付ける
- 点光源（最大 3 灯）を当て、光の玉とにじみを描く

アニメ向けに足したもの（Look = Anime が既定。Cinematic にしてアニメ向けの値を 0 にすると原典と同じ絵）:

- 元の絵を暗くせず、光の届かない側に影色（青紫など）を乗せ、光の届く側に光を足す
- セル調の陰影（段数とぼかし）
- リムライト（輪郭の光。光源を物の後ろに置くと逆光になる）
- 線画の保護（主線や瞳を光と影から外し、元の色のまま残す）
- 深度の膨らみ（実写で学習した深度モデルが平たく推定した人物に丸みを付ける）
- プリセット 6 種

詳細は [docs/設計.md](docs/設計.md)。

## 構成

```
plugin/            AE エフェクト本体 (C++ / SmartFX / 32bpc / マルチフレームレンダリング対応)
  src/RelightCore.h     照明の計算そのもの (CPU と GPU で共通。TypeGPU の移植 + アニメ向け)
  src/RelightGPU.cu     GPU 版 (CUDA)。RelightCore.h の同じ関数を GPU で動かす
  src/RelightAE.cpp     AE とのやりとり、CPU 版の手順、パラメータ、プリセット、光源ギズモ
  src/RelightAuto.cpp   深度の自動生成と結線 (フレーム書き出し・推論の起動・読み込み)
  src/RelightAE.h / RelightAE_PiPL.r / RelightAE_Flags.h
  CMakeLists.txt / build.ps1
depth/
  relight_depth.cpp     深度推定 (Video Depth Anything Small の ONNX、CUDA / DirectML / CPU)
  build.ps1
models/            ONNX モデル (git 管理外。tools/export_vda_onnx.py --split で作る)
third_party/onnxruntime/include   ONNX Runtime 1.23.2 の C API ヘッダ
tools/
  export_vda_onnx.py   Video Depth Anything Small をエンコーダとヘッドの ONNX に書き出す
  depth_test.py        relight_depth を動画で試し、元の PyTorch 実装と比べる
  gen_passes.py        別の PC の深度サーバーへ動画を送る (--remote)。旧方式の ComfyUI API での生成も
  depth_server/        Depth Engine = Remote の深度サーバー (別の PC で動かす)
  harness/         プラグインの照明コードを AE なしで動かす検証ツール (relight_cli)
  ae_test.jsx 等   AE を使った検証スクリプト
cache/<shot>/      生成したパス (git 管理外)
docs/設計.md
```

## 必要なもの

- Windows 11、After Effects 2025 以降（開発環境は 2026 / 26.5）
- Visual Studio 2022 Build Tools（C++ ワークロード）、Ninja、CMake（`pip install cmake` で可）
- **After Effects SDK**: https://developer.adobe.com/after-effects/ から入手し、`C:\dev\AE_SDK\AfterEffectsSDK_26.5_win` などに展開。`build.ps1 -SdkRoot` で指定
- 深度推定 (engine=native、既定)
  - ONNX モデル: `python tools/export_vda_onnx.py --split` で `models/` に作る（元の重みは `video_depth_anything_vits.pth`、Apache-2.0）
  - CUDA 版 ONNX Runtime 1.23 の DLL（いまは ComfyUI の Python 環境に入っているものを参照）と、CUDA 12 / cuDNN 9 の DLL
    （CUDA Toolkit 12.8 と、PyTorch の `torch\lib`）。場所は設定ファイルの `ort` と `dll_dirs`
  - CUDA で失敗したときは、AE 同梱の ONNX Runtime（DirectML）で自動的にやり直す
  - ComfyUI を起動しておく必要はない
- 旧方式 (engine=comfyui): ComfyUI（稼働中）と VideoHelperSuite / MoGe / Video-Depth-Anything のノード

## 使い方

### 1. ビルドと導入

```powershell
.\plugin\build.ps1 -SdkRoot C:\dev\AE_SDK\AfterEffectsSDK_26.5_win -Install
```

管理者権限で `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\SRLM\` に `RelightAnime.aex` (エフェクト) と `RelightAnimeHelper.aex` (補助 AEGP) の 2 つが配置される。補助 AEGP が無いと自動生成が不安定になる。
ビルド時に設定ファイル `%APPDATA%\SRLM\RelightAnime.ini` へ、無い項目だけを追記する（推論プログラム、モデル、ONNX Runtime、CUDA の DLL、キャッシュの場所）。
深度推定プログラムは `.\depth\build.ps1`、ONNX モデルは `python tools\export_vda_onnx.py --split` で作る。

### 2. AE での手順

1. 動画か画像のフッテージレイヤーに「SRLM > Relight Anime」を適用する
2. AE が素材のフレームを書き出し（短辺 518px 以上に縮小）、relight_depth.exe が深度を推定する。
   完了するとコンポに非表示のレイヤー「Relight depth」が追加されて結線される
   （192 フレームの 480x864 動画で、書き出し後の推論は約 7〜10 秒。同じ素材は `cache/` に残るので 2 回目以降はすぐ結線される。
   進行は `cache/relight_auto.log` と `cache/<素材>/status.json`）
3. エフェクトを選択すると、コンポビューに Light Position の十字が出る。ドラッグして光源を動かし、Light Height で手前・奥を調整する。
   十字の周りには光源ごとに光の色の輪と「番号 h 高さ」が出る。輪の大きさは光の玉の見かけの大きさ（高いほど大きい）、物の後ろ（高さが負）なら破線
4. 雰囲気を変えるときは一番上の Preset から選ぶ（選ぶと各値が書き換わり、表示は Select... に戻る）

手動で生成する場合は `python tools/gen_passes.py --video <素材> --shot <名前>`。

### 光源をキャラクターの後ろに置く（逆光）

Light Height を負の値（例 −0.25）にすると、光源が手前の人物より奥・背景より手前に入る。
光の玉は人物に隠れ、人物の周りの背景だけがにじみで光り、人物の正面は影色になり、輪郭がリムライトで光る。
プリセット「Sunset Backlight」がこの形。

### キャッシュの整理

Auto の「Clean Cache...」を押すと、今のプロジェクトで使っていない深度キャッシュの件数と容量を出し、確認のうえ削除する。
使用中のキャッシュと生成中のものは残す。ほかのプロジェクトで使っていたキャッシュを消した場合は、
そのプロジェクトを開いたときに深度の素材が見つからないことを検出して自動で作り直す（Auto Passes が有効なとき）。

## パラメータ

| グループ | パラメータ | 意味 |
|---|---|---|
| (先頭) | Preset | Anime Standard（既定）/ Soft Daylight / Sunset Backlight / Night Neon / Cel Hard Light / Cinematic (TypeGPU) |
| Auto | Auto Passes / Generate Passes | 適用時の自動生成。ボタンで再生成 |
| | Clean Cache... | 使っていない深度キャッシュを確認のうえ削除 |
| | Depth Engine | 深度を作る場所。Generate Passes を押したときに使う。Local GPU（この PC、小さいモデル、8 秒の動画で十数秒）/ Remote Fast（別の PC の深度サーバー、小さいモデル、約 45 秒）/ Remote Quality（同じく大きいモデル vitl、約 2 分 20 秒。揺らぎが約 3 分の 1）。Remote はこの PC の GPU を使わない。つながらなければ Local GPU で作り直す |
| Light | Light Position | 光源の位置（ドラッグ可） |
| | Light Height | 光源の手前・奥。0 が最も手前の面、負なら物の後ろ（逆光） |
| | Light Color / Intensity | 光の色と強さ |
| | Range | 光の届く範囲の倍率（1 = 既定。上げると遠くまで明るく、下げると光源の近くだけ）。コンポビューに点線の輪で目安を描く |
| | Show Light | この光源の光の玉とにじみを描く |
| | Rim（グループ） | この光源のリムライト。中身は下の「Rim」の表。Light 1 は以前の Anime Style のリムの値を引き継ぐ |
| Light 2 / Light 3 | Enable / Position / Height / Color / Intensity / Range / Show Light / Rim | 2 灯目・3 灯目（既定は無効。Show Light の既定はオフ）。位置は画面の外にも置ける（光の玉を見せない補助光） |
| Scene | Look | Anime（元の色を保つ）/ Cinematic（原典: 全体を暗くしてフィルム風トーンマップ） |
| | Ambient / Ambient Color | 元映像の明るさと色味。下げるほど光が際立つ |
| | Relief | 深度から作る凹凸の強さ |
| | Specular | 照り返し |
| | Shadow | 落ち影の濃さ |
| | Occlusion | 凹みの暗さ |
| Anime Style | Form Shading | 面の向きによる立体的な陰影の割合。0 = 撮影寄り (光源を中心にした画面上のグラデーション。時間で揺れない)、1 = 立体的 (形に沿うが動画で揺れやすい)。既定 0.25 |
| | Light Spread | 撮影寄りのグラデーションの広がり (画面の高さ = 1。既定 0.7) |
| | Shadow Color / Shadow Tint | 光の届かない側に乗せる影色と強さ |
| | Edge Dither | 輪郭の近くだけ、効果を輪郭に沿う向きにならし、読み取り位置をずらして段を散らす。影やリムの縁のギザギザが消える |
| | Shade Smoothness | 陰影をならす。深度の細かい凹凸を拾わず、輪郭はまたがずに大きな面で効かせる（撮影でかぶせる影のように、トーンが均一で境目にボケ足がある） |
| | Cel Shading / Cel Steps / Cel Softness | セル調の強さ、影の段数、段の境目のぼかし |
| | Line Protect / Line Threshold | 線画を元の色のまま残す強さと、線とみなす暗さ（Output = Line Mask で確認） |
| | Depth Volume | 深度の膨らみ。平たい人物に丸みを付ける |
| Passes | Depth Layer / Depth Near Is | 深度レイヤー（近い = 白が既定） |
| | Depth Stabilize | 深度を前後のフレームで時間方向にならす (Off / 1 Frame / 2 Frames、既定 2)。動いている物の輪郭はならさない |
| | Snap Depth To Lines | 深度の境目を素材の線画に合わせる（既定オン）。影やリムの縁が線画に沿う |
| | Normal Source / Normal Layer | 面の向きを深度から作るか、法線レイヤーを使うか |
| | Pass Encoding | Linear（PNG または Preserve RGB 済み EXR）/ sRGB (decode)（AE が EXR を変換した場合） |
| | Mask Layer / Mask Source | 効果を限定するマスク |
| | Input Is Linear | リニア作業空間のとき |
| | Use GPU | GPU (CUDA) で描く（既定オン）。AE のプロジェクト設定が Mercury GPU アクセラレーション (CUDA) のときだけ効く |
| Output | Relit / Depth / Normals / Shadow & Occlusion / Line Mask | 出力種別 |
| | Mix | 適用量 |

Rim（各 Light の中。光源ごとに設定する）

| パラメータ | 意味 |
|---|---|
| Use Rim | この光源でリムライトを付けるか（切ると面を照らすだけの光源になる） |
| Rim Light / Rim Width | 輪郭の光の強さと幅（1080p でのピクセル数） |
| Rim Softness | 0 でくっきりした帯、上げると内側がなだらかに消える |
| Rim Smoothing | 輪郭をならしてから帯を作る（深度の中央値フィルター）。深度推定の細かい揺れや粗い解像度の階段を消し、アニメのようなすっとした線にする。既定 0.5 |
| Rim Placement | Auto（光源が手前なら光源の側だけ控えめに、物の後ろなら輪郭全体）/ Light Side / All Around |
| Rim Color | リムの色（光源の色に掛ける。白なら光源の色のまま） |
| Rim Spread | 光源側の輪郭からどこまで回り込んで光らせるか（0 = 光源を向いた側だけ、1 = 輪郭全体。既定 0.35） |
| Rim Reach | 画面上で光源からどこまで光らせるか（画面の高さ = 1、3 = 制限なし。既定 0.9） |

リムが乗る画素では、Line Protect は線のはっきりした芯だけを守る（輪郭線の半端な縁まで守ると、リムの帯が細かく欠けてガタガタに見えるため）。

プリセットの見比べは `python tools/harness/compare_presets.py --video <素材> --frame <番号> --depth <深度 PNG> --out <出力>`。

## 現状

- 照明は AE 外の検証ツール（プラグインと同じコード）で静止画・動画フレームとも確認済み
- 自動生成（書き出し→推論→読み込み→結線）は AE 2026 で確認済み（補助 AEGP 経由）
- 処理速度 (1344x768、全部入りのプリセット): GPU (RTX 5090、CUDA) で約 0.6〜1.3 ms、CPU (24 スレッド) で約 40〜50 ms。GPU 版と CPU 版の差は 8bit で最大 1 段 (ごく一部の画素)
- GPU で描くには、AE の「ファイル > プロジェクト設定 > ビデオレンダリングおよびエフェクト」を Mercury GPU アクセラレーション (CUDA) にする。それ以外 (ソフトウェアのみ・DirectX など) では CPU 版で描く
- 生成型リライト（LiveLight）は見送り（2026-10-07）。TypeGPU 方式で進める

### Depth Engine = Remote（別の PC の深度サーバー）

- 別の PC（例: Linux の GPU マシン）で深度サーバー（`tools/depth_server/depth_server.py`、ComfyUI 不要）を動かし、深度を作らせる。動画を `POST /depth?model=vits|vitl` で受け取り、Video Depth Anything（公式のコード）→ ショット全体の 2%〜98% で正規化 → 時間方向のならしまで済ませた 16bit の深度（npz）を返す。
- systemd のソケット起動で、依頼が来たときだけ起動し、5 分なにも来なければ自分で終わる（待っている間はメモリも GPU も使わない）。`127.0.0.1` だけで待ち受け、外へは Tailscale Serve などで必要な範囲だけに公開する。
- 設置の手順と systemd の雛形は [tools/depth_server/README.md](tools/depth_server/README.md)。
- 送り先は `%APPDATA%\SRLM\RelightAnime.ini` の `remote_server`（`plugin\build.ps1 -RemoteServer <URL>` で書ける）。空ならこの PC で作る。
- 目安（GB10、8 秒・192 コマ）: 小さいモデル（Fast）約 45 秒、大きいモデル（Quality）約 2 分 20 秒。

## ライセンス

このプロジェクトは [MIT ライセンス](LICENSE) です。他者のソフトウェアの表示は [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) にあります。

照明モデルは TypeGPU（MIT、Software Mansion）からの移植です。Video Depth Anything の Small は Apache-2.0、**Large は CC-BY-NC-4.0（非商用）** で、Remote Quality で使います。After Effects SDK は含めていません。詳しくは [docs/how-it-works.ja.md](docs/how-it-works.ja.md#10-ライセンスの注意)。
