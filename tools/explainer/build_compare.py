#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build_compare.py - 元の動画と Relight Anime をかけた動画を左右に並べた比較動画 (ffmpeg)

  python build_compare.py   → out/relight_compare.mp4 (1920x1080, 30fps)
左 = renders/Normal.mp4、右 = Relit_1_Cinematic → Relit_2_Anime → Relit_3_Anime を 8 秒ずつ。
"""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent
R, SEG, OUT, TXT = ROOT / "renders", ROOT / "segments_compare", ROOT / "out", ROOT / "text"
for d in (SEG, OUT, TXT):
    d.mkdir(exist_ok=True)
FONT_B = "C\\:/Windows/Fonts/BIZ-UDGothicB.ttc"
FPS = 30
ENC = ["-c:v", "libx264", "-preset", "medium", "-crf", "16", "-pix_fmt", "yuv420p", "-r", str(FPS),
       "-c:a", "aac", "-b:a", "128k", "-ar", "48000", "-ac", "2"]


def text_file(name, s):
    p = TXT / f"{name}.txt"
    p.write_text(s, encoding="utf-8")
    return str(p).replace("\\", "/").replace(":", "\\:")


def label(name, s, x):
    return (f"drawbox=x={x}:y=207:w=956:h=58:color=black@0.6:t=fill,"
            f"drawtext=fontfile='{FONT_B}':textfile='{text_file(name, s)}':fontsize=36:fontcolor=white:x={x}+(956-tw)/2:y=219")


def pair(n, right, right_label, dur=8.0):
    fl = (f"[0:v]scale=956:534:flags=lanczos,fps={FPS}[l];[1:v]scale=956:534:flags=lanczos,fps={FPS}[r];"
          f"[l][r]hstack=inputs=2[s];[s]pad=1920:1080:4:(1080-534)/2:color=0x101418,"
          f"{label(f'cmp{n}_l', '元の動画', 4)},{label(f'cmp{n}_r', right_label, 960)},"
          f"fade=t=in:st=0:d=0.3,fade=t=out:st={dur - 0.3}:d=0.3[out]")
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-t", f"{dur}", "-i", str(R / "Normal.mp4"), "-t", f"{dur}", "-i", str(R / f"{right}.mp4"),
                    "-f", "lavfi", "-t", f"{dur}", "-i", "anullsrc=r=48000:cl=stereo",
                    "-filter_complex", fl, "-map", "[out]", "-map", "2:a", "-shortest"] + ENC + [str(SEG / f"{n:02d}.mp4")], check=True)


def main():
    pair(1, "Relit_1_Cinematic", "Relight Anime（Cinematic）")
    pair(2, "Relit_2_Anime", "Relight Anime（Anime）")
    pair(3, "Relit_3_Anime", "Relight Anime（Anime・別の設定）")
    segs = sorted(SEG.glob("*.mp4"))
    lst = SEG / "list.txt"
    lst.write_text("".join(f"file '{p.as_posix()}'\n" for p in segs), encoding="utf-8")
    out = OUT / "relight_compare.mp4"
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-f", "concat", "-safe", "0", "-i", str(lst), "-c", "copy",
                    "-movflags", "+faststart", str(out)], check=True)
    print(f"保存: {out}")


if __name__ == "__main__":
    main()
