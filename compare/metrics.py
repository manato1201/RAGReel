"""Phase 0: 3方式(Qt / HyperFrames / Python)の「同じ入力」比較表を作る。

測るもの: 生成時間 / ファイルサイズ / 字幕のずれ / 実装行数 / 出力長・解像度・fps。
字幕のずれは、各動画の音声そのものから発話開始を検出(common.subtitles.speech_onsets)し、
その動画に付いた字幕の開始時刻と突き合わせた平均・最大(秒)。同じ基準で3方式を測る。

  python metrics.py --qt-dir out/qt_run --qt-log out/qt_run/qt.log --out out/metrics.json
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
from common import subtitles  # noqa: E402

ROOT = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(ROOT)


def probe(path: str) -> dict:
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries",
                          "stream=codec_type,width,height,r_frame_rate:format=duration,size",
                          "-of", "json", path], capture_output=True, text=True, check=True).stdout
    j = json.loads(out)
    v = next((s for s in j["streams"] if s.get("codec_type") == "video"), {})
    num, _, den = (v.get("r_frame_rate", "0/1")).partition("/")
    return {"duration": round(float(j["format"]["duration"]), 2), "size_bytes": int(j["format"]["size"]),
            "width": v.get("width"), "height": v.get("height"),
            "fps": round(float(num) / float(den or 1), 2) if den and float(den) else None}


def extract_audio(video: str, wav: str):
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", video, "-vn", "-ac", "1", "-ar", "44100",
                    "-c:a", "pcm_s16le", wav], check=True)


def parse_srt(path: str) -> list:
    segs = []
    blocks = re.split(r"\n\s*\n", open(path, encoding="utf-8").read().strip())
    pat = re.compile(r"(\d+):(\d+):(\d+)[,.](\d+)\s*-->\s*(\d+):(\d+):(\d+)[,.](\d+)")
    for b in blocks:
        m = pat.search(b)
        if not m:
            continue
        g = list(map(int, m.groups()))
        segs.append({"start": g[0] * 3600 + g[1] * 60 + g[2] + g[3] / 1000,
                     "end": g[4] * 3600 + g[5] * 60 + g[6] + g[7] / 1000,
                     "text": " ".join(b.splitlines()[2:])})
    return segs


def subtitle_offset(video: str, segs: list) -> dict:
    with tempfile.TemporaryDirectory() as d:
        wav = os.path.join(d, "a.wav")
        extract_audio(video, wav)
        return subtitles.measure_offset(segs, subtitles.speech_onsets(wav))


def count_lines(paths) -> int:
    n = 0
    for p in paths:
        with open(p, encoding="utf-8", errors="ignore") as f:
            n += sum(1 for line in f if line.strip() and not line.strip().startswith(("#", "//")))
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qt-dir")
    ap.add_argument("--qt-log")
    ap.add_argument("--qt-wall", type=float, help="Qt版の生成時間(秒)。ログの Stage 合計を使うなら省略")
    ap.add_argument("--py-video", default=os.path.join(ROOT, "out", "python.mp4"))
    ap.add_argument("--py-wall", type=float)
    ap.add_argument("--hf-video", default=os.path.join(ROOT, "out", "hyperframes.mp4"))
    ap.add_argument("--hf-wall", type=float)
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "metrics.json"))
    a = ap.parse_args()

    rows = {}
    # --- Python ---
    if os.path.exists(a.py_video):
        info = probe(a.py_video)
        srt = os.path.splitext(a.py_video)[0] + ".srt"
        j = json.load(open(os.path.splitext(a.py_video)[0] + ".json", encoding="utf-8"))
        rows["Python"] = {**info, "seconds": a.py_wall or j["seconds"], "exact_subtitles": True,
                          "subtitle_offset": subtitle_offset(a.py_video, parse_srt(srt)),
                          "loc": count_lines([os.path.join(ROOT, "python_render", "engine.py"),
                                              os.path.join(ROOT, "python_render", "render.py")])}
    # --- HyperFrames ---
    if os.path.exists(a.hf_video):
        info = probe(a.hf_video)
        srt = os.path.join(ROOT, "out", "python.srt")  # 字幕は同じ時刻情報(python版が書いたもの)を共有
        rows["HyperFrames"] = {**info, "seconds": a.hf_wall, "exact_subtitles": True,
                               "subtitle_offset": subtitle_offset(a.hf_video, parse_srt(srt)),
                               "loc": count_lines([os.path.join(ROOT, "hyperframes", "build_project.py")])}
    # --- Qt ---
    if a.qt_dir:
        webm = sorted(glob.glob(os.path.join(a.qt_dir, "*.webm")))
        if webm:
            info = probe(webm[-1])
            secs = a.qt_wall
            if secs is None and a.qt_log and os.path.exists(a.qt_log):
                secs = round(sum(float(x) for x in re.findall(r"Stage \w+: ok \(([\d.]+)s\)",
                                                              open(a.qt_log, encoding="utf-8", errors="ignore").read())), 1)
            # Qt版の字幕は概算(SAPI)。output/ に書かれた subtitle_cues を同じ基準で測る。
            cues = []
            out_videos = os.path.join(REPO, "build", "engine", "output", "videos")
            if os.path.isdir(out_videos):
                newest = max((os.path.join(out_videos, d) for d in os.listdir(out_videos)),
                             key=os.path.getmtime)
                mp = os.path.join(newest, "metadata.json")
                if os.path.exists(mp):
                    meta = json.load(open(mp, encoding="utf-8"))
                    cues = [{"start": c["start"], "end": c["end"], "text": c["text"]}
                            for c in meta.get("render", {}).get("subtitle_cues", [])]
                    exact = bool(meta.get("render", {}).get("subtitles_exact"))
            rows["Qt"] = {**info, "seconds": secs, "exact_subtitles": locals().get("exact", False),
                          "subtitle_offset": subtitle_offset(webm[-1], cues) if cues else None,
                          "loc": count_lines(glob.glob(os.path.join(REPO, "engine", "src", "**", "*.cpp"), recursive=True)
                                             + glob.glob(os.path.join(REPO, "engine", "src", "**", "*.h"), recursive=True)
                                             + glob.glob(os.path.join(REPO, "engine", "qml", "*.qml")))}
    for r in rows.values():
        if r.get("seconds") and r.get("duration"):
            r["seconds_per_video_second"] = round(r["seconds"] / r["duration"], 2)
    json.dump(rows, open(a.out, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print(json.dumps(rows, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
