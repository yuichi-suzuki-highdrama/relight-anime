#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build_explainer.py - Relight Anime の解説動画を組み立てる (ffmpeg)

  python build_explainer.py            → out/relight_explainer.mp4 (1920x1080, 30fps)

素材:
  renders/  AE の書き出し (Normal, Relit_*, demo_cinematic_*)
  capture/  AE の画面録画 (2400x1350 を 1920x1080 に縮めたもの)
場面ごとに segments/NN.mp4 を作り、最後につなぐ。文字は textfile で渡す (エスケープを避ける)。
"""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent
R, C, SEG, OUT, TXT = ROOT / "renders", ROOT / "capture", ROOT / "segments", ROOT / "out", ROOT / "text"
for d in (SEG, OUT, TXT):
    d.mkdir(exist_ok=True)
# 文字はすべて BIZ UDPゴシック (UD ゴシックのプロポーショナル版。英字・数字が等幅で間延びしない)。
# Windows の .ttc から取り出したもの (fonts/。ffmpeg の書体名指定は Windows で落ちるのでファイルで渡す)
FONT_B = "D\\:/video_projects/relight_explainer/fonts/BIZUDPGothicBold.ttf"
FONT_R = "D\\:/video_projects/relight_explainer/fonts/BIZUDPGothic.ttf"
W, H, FPS = 1920, 1080, 30
BGM = ROOT / "bgm_whistle_call.mp3"   # 背景の曲 (Whistle Call)
BGM_GAIN_DB = -17
ENC = ["-c:v", "libx264", "-preset", "medium", "-crf", "17", "-pix_fmt", "yuv420p", "-r", str(FPS),
       "-c:a", "aac", "-b:a", "128k", "-ar", "48000", "-ac", "2"]


def run(args):
    subprocess.run(["ffmpeg", "-y", "-v", "error"] + args, check=True)


def text_file(name, s):
    p = TXT / f"{name}.txt"
    p.write_text(s, encoding="utf-8")
    return str(p).replace("\\", "/").replace(":", "\\:")


def caption(name, main, sub=""):
    """画面下の帯と文字 (main は大きく、sub は小さく)"""
    f = [f"drawbox=x=0:y=ih-190:w=iw:h=190:color=black@0.62:t=fill",
         f"drawtext=fontfile='{FONT_B}':textfile='{text_file(name + '_m', main)}':fontsize=52:fontcolor=white:x=(w-tw)/2:y=h-160"]
    if sub:
        f.append(f"drawtext=fontfile='{FONT_R}':textfile='{text_file(name + '_s', sub)}':fontsize=34:fontcolor=0xDDDDDD:x=(w-tw)/2:y=h-82")
    return ",".join(f)


def fades(dur):
    return f"fade=t=in:st=0:d=0.35,fade=t=out:st={dur - 0.35:.2f}:d=0.35"


def clip(n, src, ss, dur, vf_pre, cap, speed=1.0):
    """素材の一部を切り出し、整えて文字を載せる。dur は素材側の長さ。speed 倍で再生する (画面録画の早送り用)"""
    out_dur = dur / speed
    vf = f"setpts=PTS/{speed},{vf_pre},fps={FPS},{cap},{fades(out_dur)}"
    run(["-ss", f"{ss}", "-t", f"{dur}", "-i", str(src), "-f", "lavfi", "-t", f"{out_dur}", "-i", "anullsrc=r=48000:cl=stereo",
         "-vf", vf, "-map", "0:v", "-map", "1:a", "-shortest"] + ENC + [str(SEG / f"{n:02d}.mp4")])


def card(n, dur, lines):
    """黒地の文字だけの場面。lines = [(文字, 大きさ, 太字か, y)]"""
    parts = []
    for i, (s, size, bold, y) in enumerate(lines):
        font = FONT_B if bold else FONT_R
        color = "white" if bold else "0xDDDDDD"
        parts.append(f"drawtext=fontfile='{font}':textfile='{text_file(f'card{n}_{i}', s)}':fontsize={size}:fontcolor={color}:x=(w-tw)/2:y={y}")
    vf = ",".join(parts) + "," + fades(dur)
    run(["-f", "lavfi", "-t", f"{dur}", "-i", f"color=c=0x101418:s={W}x{H}:r={FPS}",
         "-f", "lavfi", "-t", f"{dur}", "-i", "anullsrc=r=48000:cl=stereo",
         "-vf", vf, "-map", "0:v", "-map", "1:a", "-shortest"] + ENC + [str(SEG / f"{n:02d}.mp4")])


FIT = f"scale={W}:{H}:force_original_aspect_ratio=decrease:flags=lanczos,pad={W}:{H}:(ow-iw)/2:(oh-ih)/2:color=0x101418"


def grid(n, dur):
    """元の動画と 3 つの照明を 2x2 で並べる (ラベル付き)"""
    names = [("Normal", "元の動画"), ("Relit_1_Cinematic", "Cinematic"), ("Relit_2_Anime", "Anime"), ("Relit_3_Anime", "Anime（別の設定）")]
    ins, fl = [], []
    for i, (f, label) in enumerate(names):
        ins += ["-t", f"{dur}", "-i", str(R / f"{f}.mp4")]
        fl.append(f"[{i}:v]scale=944:527:flags=lanczos,fps={FPS},"
                  f"drawbox=x=0:y=0:w=iw:h=54:color=black@0.55:t=fill,"
                  f"drawtext=fontfile='{FONT_B}':textfile='{text_file(f'grid{i}', label)}':fontsize=32:fontcolor=white:x=16:y=11[v{i}]")
    fl.append("[v0][v1]hstack=inputs=2:shortest=1[top];[v2][v3]hstack=inputs=2:shortest=1[bot];[top][bot]vstack=inputs=2[g]")
    fl.append(f"[g]pad={W}:{H}:(ow-iw)/2:(oh-ih)/2:color=0x101418,"
              f"{caption('grid', '1本の動画に違ったライティングができる', 'After Effects のエフェクトとして、元の映像に光と影を足す')},{fades(dur)}[out]")
    # 音は元の動画のもの (4 本とも同じ音)
    fl.append(f"[0:a]atrim=0:{dur},asetpts=PTS-STARTPTS,aresample=48000,afade=t=in:st=0:d=0.35,afade=t=out:st={dur - 0.35:.2f}:d=0.35[aout]")
    run(ins + ["-filter_complex", ";".join(fl),
               "-map", "[out]", "-map", "[aout]"] + ENC + [str(SEG / f"{n:02d}.mp4")])


def slide(t_in, t_out, d=0.5, x0=72):
    """drawtext の x の式: t_in から d 秒で左の外から x0 へ入り (ease-out)、t_out から d 秒で左の外へ出る (ease-in)"""
    a = f"(1-pow(1-(t-{t_in})/{d},3))"   # 入り: 0→1
    b = f"pow((t-{t_out})/{d},3)"         # 出: 0→1
    off = f"(-tw-80)"
    return (f"if(lt(t,{t_in}),{off},if(lt(t,{t_in + d}),{off}+({x0}-{off})*{a},"
            f"if(lt(t,{t_out}),{x0},if(lt(t,{t_out + d}),{x0}+({off}-{x0})*{b},{off}))))")


def reel(n, step=2.0):
    """冒頭: 4 つの動画を画面いっぱいで、1 回の再生 (8 秒) の間に step 秒ごとに切り替える (時間は途切れず進む)"""
    names = [("Normal", "元の動画"), ("Relit_2_Anime", "Relight Anime（Anime）"),
             ("Relit_3_Anime", "Relight Anime（Anime・別の設定）"), ("Relit_1_Cinematic", "Relight Anime（Cinematic）")]
    dur = step * len(names)
    ins, fl = [], []
    for i, (f, label) in enumerate(names):
        ins += ["-i", str(R / f"{f}.mp4")]
        fl.append(f"[{i}:v]trim=start={i * step}:duration={step},setpts=PTS-STARTPTS,{FIT},fps={FPS},"
                  f"drawbox=x=40:y=36:w=tw_placeholder:h=0:color=black@0:t=fill,"
                  f"drawtext=fontfile='{FONT_B}':textfile='{text_file(f'reel{i}', label)}':fontsize=44:fontcolor=white:"
                  f"box=1:boxcolor=black@0.55:boxborderw=18:x=56:y=48[v{i}]")
    fl = [s.replace("drawbox=x=40:y=36:w=tw_placeholder:h=0:color=black@0:t=fill,", "") for s in fl]
    fl.append("".join(f"[v{i}]" for i in range(len(names))) + f"concat=n={len(names)}:v=1:a=0[c]")
    # タイトル: 左下に、画面の外からすべり込んで (終わりがゆっくり)、少し見せて、外へすべり出る
    title = (f"drawtext=fontfile='{FONT_B}':textfile='{text_file('reel_title', 'Relight Anime')}':fontsize=96:fontcolor=white:"
             f"box=1:boxcolor=black@0.5:boxborderw=22:y=h-300:x='{slide(0.6, 3.6)}'")
    sub = (f"drawtext=fontfile='{FONT_R}':textfile='{text_file('reel_sub', 'AI アニメ動画に、あとから光を足す After Effects プラグイン')}':"
           f"fontsize=42:fontcolor=white:box=1:boxcolor=black@0.5:boxborderw=16:y=h-160:x='{slide(0.8, 3.4)}'")
    fl.append(f"[c]{title},{sub},fade=t=in:st=0:d=0.35,fade=t=out:st={dur - 0.35}:d=0.35[out]")
    # 音は元の動画を 1 回分そのまま通す (4 本とも同じ音なので、絵が切り替わっても音は途切れない)
    fl.append(f"[0:a]atrim=0:{dur},asetpts=PTS-STARTPTS,aresample=48000,afade=t=in:st=0:d=0.35,afade=t=out:st={dur - 0.35:.2f}:d=0.35[aout]")
    run(ins + ["-filter_complex", ";".join(fl),
               "-map", "[out]", "-map", "[aout]"] + ENC + [str(SEG / f"{n:02d}.mp4")])


def main():
    for old in SEG.glob("*.mp4"):
        old.unlink()
    reel(0)
    # 深度: Output を Relit → Depth → Shadow & Occlusion → Relit (エフェクトの設定とコンポの画面を切り出す)
    clip(3, C / "cap_output_switch.mkv", 9.5, 16.0, f"crop=972:547:0:150,{FIT}",
         caption("depth", "動画から奥行き（深度）を自動で推定して、光と影を計算", "Output：Relit → Depth（深度）→ Shadow & Occlusion（影と凹み）　［2 倍速］"),
         speed=2.0)
    clip(5, R / "demo_cinematic_light_sweep.mp4", 0, 8.0, FIT,
         caption("sweep", "ライトの位置に合わせて、影が移り変わる", "Look：Cinematic ／ リムライト：オフ"))
    clip(6, R / "demo_cinematic_height_sweep.mp4", 0, 8.0, FIT,
         caption("height", "高さを下げると人物の後ろへ — 逆光で顔が沈む", "Light Height：手前 1.2 → 後ろ −0.3 → 手前"))
    grid(8, 7.0)   # 4 本を 2x2 で並べて締める

    segs = sorted(SEG.glob("*.mp4"))
    lst = SEG / "list.txt"
    lst.write_text("".join(f"file '{p.as_posix()}'\n" for p in segs), encoding="utf-8")
    joined = OUT / "joined_nobgm.mp4"
    run(["-f", "concat", "-safe", "0", "-i", str(lst), "-c", "copy", str(joined)])
    # 背景の曲をうっすら敷く (BGM_GAIN_DB 下げる、頭と終わりをフェード)。場面の音 (冒頭と 4 分割の元の動画の音) はそのまま重ねる
    total = float(subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", str(joined)],
                                 capture_output=True, text=True, check=True).stdout.strip())
    out = OUT / "relight_explainer.mp4"
    af = (f"[1:a]atrim=0:{total},asetpts=PTS-STARTPTS,aresample=48000,volume={BGM_GAIN_DB}dB,"
          f"afade=t=in:st=0:d=1.5,afade=t=out:st={total - 2.5:.2f}:d=2.5[bgm];"
          f"[0:a][bgm]amix=inputs=2:duration=first:normalize=0[a]")
    run(["-i", str(joined), "-i", str(BGM), "-filter_complex", af, "-map", "0:v", "-map", "[a]",
         "-c:v", "copy", "-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart", str(out)])
    joined.unlink()
    print(f"保存: {out} ({total:.1f}s)")


if __name__ == "__main__":
    main()
