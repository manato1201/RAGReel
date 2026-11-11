#!/usr/bin/env python
"""書き出した動画の検査(QUALITY_UPLIFT QL5 / Z79 の「測る道具」)。

「自分で作った動画を自分で採点しない」ために、動画そのものを測る。測るもの:
  1. 音声と映像の長さが一致するか(例: 映像 335.800 秒に音声 332.053 秒、は不合格)
  2. 配色が、エンジンが宣言した目標色のとおりか(ΔE。時刻を変えて測り、途中での色のずれも捕まえる)
  3. 途中の黒いフレーム・音割れ(クリップ)・無音
  4. 字幕・章(あれば)の時刻の整合
  5. コンタクトシート(拍ごとの静止画 + 切り替わりの前後のフレーム)= **人が見る**ための画像
  6. 決定性(任意): 同じ入力を 2 回書き出した 2 本の動画が、同じフレームか

既存の検査と重ねない: 字幕の読み込みは compare/metrics.py の parse_srt を使う(長さ・字幕のずれの**比較**は metrics.py の仕事。
ここは、1 本ごとの**合否**)。

使い方:
  python tools/verify_export.py web/public/videos/<id>                # 1 本(video.mp4 + metadata.json + 字幕)
  python tools/verify_export.py out/video.mp4 --contact-sheet sheet.png
  python tools/verify_export.py web/public/videos --all               # 全部
  python tools/verify_export.py a.mp4 --twin b.mp4                    # 同じ入力の 2 回の書き出し
終了コード: 0 = 不合格なし(警告はあってよい) / 1 = 不合格あり / 2 = 使い方の誤り・ツールが無い
"""
from __future__ import annotations

import argparse
import importlib.util
import io
import json
import math
import os
import re
import subprocess
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SPEC = os.path.join(HERE, "video_spec.json")

PASS, WARN, FAIL, SKIP = "pass", "warn", "fail", "skip"


class Check:
    def __init__(self, name: str, status: str, detail: str, **data):
        self.name, self.status, self.detail, self.data = name, status, detail, data

    def as_dict(self) -> dict:
        return {"name": self.name, "status": self.status, "detail": self.detail, **({"data": self.data} if self.data else {})}


def _run(cmd: list[str], binary: bool = False) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=not binary, check=False)


def need_tools() -> None:
    for tool in ("ffmpeg", "ffprobe"):
        try:
            _run([tool, "-version"])
        except FileNotFoundError:
            print(f"{tool} が見つかりません(PATH を確認してください)", file=sys.stderr)
            sys.exit(2)


# ----------------------------------------------------------------------------- 測定の部品
def probe(path: str) -> dict:
    out = _run(["ffprobe", "-v", "error", "-show_entries",
                "stream=codec_type,codec_name,width,height,r_frame_rate,duration:format=duration",
                "-of", "json", path])
    if out.returncode != 0:
        raise RuntimeError(f"ffprobe が読めません: {out.stderr.strip()[:200]}")
    j = json.loads(out.stdout)
    video = next((s for s in j["streams"] if s.get("codec_type") == "video"), None)
    audio = next((s for s in j["streams"] if s.get("codec_type") == "audio"), None)
    fmt = float(j["format"]["duration"])

    def dur(s):
        try:
            return float(s["duration"])
        except (KeyError, TypeError, ValueError):
            return None

    fps = None
    if video:
        num, _, den = str(video.get("r_frame_rate", "0/1")).partition("/")
        fps = float(num) / float(den) if den and float(den) else None
    return {"format_duration": fmt, "video": video, "audio": audio,
            "video_duration": dur(video) if video else None, "audio_duration": dur(audio) if audio else None, "fps": fps}


def frame_at(path: str, t: float) -> Image.Image:
    out = _run(["ffmpeg", "-v", "error", "-ss", f"{max(t, 0):.3f}", "-i", path, "-frames:v", "1",
                "-f", "image2pipe", "-vcodec", "png", "-"], binary=True)
    if out.returncode != 0 or not out.stdout:
        raise RuntimeError(f"{t:.2f} 秒のフレームを取り出せません")
    return Image.open(io.BytesIO(out.stdout)).convert("RGB")


def _srgb_to_lab(rgb):
    def lin(c):
        c /= 255
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
    r, g, b = (lin(c) for c in rgb)
    x = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047
    y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b
    z = (0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / 1.08883
    f = lambda t: t ** (1 / 3) if t > 216 / 24389 else (24389 / 27 * t + 16) / 116
    fx, fy, fz = f(x), f(y), f(z)
    return 116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)


def delta_e(a, b) -> float:
    """CIE76 の色差(2 色の見た目の違い。1 前後は気づかない、3 以上は気づく程度)。"""
    la, lb = _srgb_to_lab(a), _srgb_to_lab(b)
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(la, lb)))


def hex_to_rgb(h: str):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def region_mean(im: Image.Image, region) -> tuple:
    w, h = im.size
    x0, y0, x1, y1 = (int(region[0] * w), int(region[1] * h), max(int(region[2] * w), int(region[0] * w) + 1), max(int(region[3] * h), int(region[1] * h) + 1))
    px = im.crop((x0, y0, x1, y1)).resize((1, 1), Image.BOX).getpixel((0, 0))
    return px


def resolve_time(spec_t, duration: float) -> float:
    if isinstance(spec_t, (int, float)):
        return min(float(spec_t), max(duration - 0.1, 0))
    if spec_t == "mid":
        return duration / 2
    m = re.fullmatch(r"end-(\d+(?:\.\d+)?)", str(spec_t))
    if m:
        return max(duration - float(m.group(1)), 0)
    raise ValueError(f"時刻の書き方が不正です: {spec_t!r}(数値・\"mid\"・\"end-1\" など)")


# ----------------------------------------------------------------------------- 検査
def check_streams(info: dict, spec: dict) -> Check:
    problems = []
    if not info["video"]:
        problems.append("映像のストリームがありません")
    if not info["audio"]:
        problems.append("音声のストリームがありません")
    frame = spec.get("frame", {})
    v = info["video"] or {}
    if frame.get("width") and v.get("width") and (v["width"], v["height"]) != (frame["width"], frame["height"]):
        problems.append(f"解像度 {v['width']}x{v['height']}(期待 {frame['width']}x{frame['height']})")
    legacy = False
    if frame.get("fps") and info["fps"] and abs(info["fps"] - frame["fps"]) > 0.01:
        if any(abs(info["fps"] - f) <= 0.01 for f in spec.get("legacyFps", [])):
            legacy = True  # 旧版の書き出し(エンジンが 30fps だった頃)。不合格にはしない
        else:
            problems.append(f"fps {info['fps']:.2f}(期待 {frame['fps']})")
    if problems:
        return Check("streams", FAIL, "・".join(problems))
    if legacy:
        return Check("streams", WARN, f"fps {info['fps']:.0f}(旧版。現在のエンジンは {frame['fps']}fps)")
    return Check("streams", PASS, f"{v.get('codec_name')} {v.get('width')}x{v.get('height')} {info['fps']:.0f}fps + 音声あり")


def check_av_duration(info: dict, spec: dict) -> Check:
    """映像と音声の長さ。エンジンは、ナレーションの後ろに**意図した無音の余白**(kTailPaddingSeconds = 1.5 秒。engine/src/main_cloudrag.cpp)を
    付けて映像を作る。だから、映像 − 音声 は 0 〜 余白 の間が正しい。音声が映像より長い(末尾が切れる)・余白を超えて映像が長い(声が途切れた後に
    無音の映像が続く)は、不合格。"""
    tol = spec.get("durationToleranceSec", 0.10)
    tail = spec.get("expectedTailSec", 0.0)
    vd, ad = info["video_duration"] or info["format_duration"], info["audio_duration"]
    if ad is None:
        return Check("av-duration", SKIP, "音声の長さが取れません(音声なし)")
    gap = vd - ad  # 正 = 映像のほうが長い
    detail = f"映像 {vd:.3f} 秒・音声 {ad:.3f} 秒(映像 − 音声 = {gap:+.3f} 秒。正しい範囲 {-tol:+.2f} 〜 {tail + tol:+.2f} 秒)"
    if gap < -tol:
        return Check("av-duration", FAIL, detail + "。音声が映像より長い(末尾が切れる)", video=vd, audio=ad, gap=gap)
    if gap > tail + tol:
        return Check("av-duration", FAIL, detail + f"。余白(宣言 {tail:.1f} 秒)を {gap - tail:.2f} 秒超えて、声が終わった後も映像が続く", video=vd, audio=ad, gap=gap, excess=gap - tail)
    return Check("av-duration", PASS, detail, video=vd, audio=ad, gap=gap)


def check_colors(path: str, duration: float, spec: dict) -> Check:
    items = spec.get("colors", [])
    if not items:
        return Check("colors", SKIP, "色の目標値が指定されていません")
    worst, failures, n = 0.0, [], 0
    for item in items:
        target = hex_to_rgb(item["rgb"])
        limit = item.get("deltaE", 3.0)
        for spec_t in item.get("times", [1.0]):
            t = resolve_time(spec_t, duration)
            got = region_mean(frame_at(path, t), item["region"])
            de = delta_e(got, target)
            n += 1
            worst = max(worst, de)
            if de > limit:
                failures.append(f"{item['name']} @{t:.1f}s: 測定 #{got[0]:02x}{got[1]:02x}{got[2]:02x} / 目標 {item['rgb']} / ΔE {de:.1f}(許容 {limit})")
    if failures:
        return Check("colors", FAIL, "; ".join(failures), worst=worst)
    return Check("colors", PASS, f"{n} か所すべて目標色の許容内(最大 ΔE {worst:.2f})", worst=worst)


def check_black(path: str, duration: float, spec: dict) -> Check:
    out = _run(["ffmpeg", "-v", "info", "-i", path, "-an", "-vf", "blackdetect=d=0.5:pic_th=0.98", "-f", "null", "-"])
    segs = [(float(a), float(b)) for a, b in re.findall(r"black_start:([\d.]+) black_end:([\d.]+)", out.stderr)]
    edge = spec.get("blackEdgeAllowedSec", 1.0)
    bad = [(s, e) for s, e in segs if not ((s < 0.2 and e - s <= edge) or (e >= duration - 0.2 and e - s <= edge))]
    if bad:
        return Check("black-frames", FAIL, "途中の黒いフレーム: " + ", ".join(f"{s:.1f}〜{e:.1f}秒" for s, e in bad), segments=bad)
    return Check("black-frames", PASS, "途中の黒いフレームなし" + (f"(端の黒 {len(segs)} 件は許容)" if segs else ""))


def check_audio_levels(path: str, spec: dict) -> Check:
    out = _run(["ffmpeg", "-v", "info", "-i", path, "-vn", "-af", "volumedetect", "-f", "null", "-"])
    mean = re.search(r"mean_volume:\s*(-?[\d.]+) dB", out.stderr)
    peak = re.search(r"max_volume:\s*(-?[\d.]+) dB", out.stderr)
    if not mean or not peak:
        return Check("audio-levels", SKIP, "音声のレベルを測れません")
    mean_db, peak_db = float(mean.group(1)), float(peak.group(1))
    problems = []
    if peak_db >= spec.get("clipPeakDb", -0.1):
        problems.append(f"音割れ(ピーク {peak_db:.1f} dBFS が上限に張り付いている)")
    if mean_db <= spec.get("silentMeanDb", -50.0):
        problems.append(f"ほぼ無音(平均 {mean_db:.1f} dBFS)")
    if problems:
        return Check("audio-levels", FAIL, "・".join(problems), mean_db=mean_db, peak_db=peak_db)
    return Check("audio-levels", PASS, f"平均 {mean_db:.1f} dBFS・ピーク {peak_db:.1f} dBFS", mean_db=mean_db, peak_db=peak_db)


def _parse_cues(path: str) -> list[tuple[float, float]]:
    """字幕(.srt / .vtt)の開始・終了(秒)。compare/metrics.py の parse_srt を使う(重複して書かない)。"""
    spec = importlib.util.spec_from_file_location("rr_compare_metrics", os.path.join(HERE, "..", "compare", "metrics.py"))
    mod = importlib.util.module_from_spec(spec)
    sys.path.insert(0, os.path.join(HERE, "..", "compare"))
    try:
        spec.loader.exec_module(mod)  # type: ignore[union-attr]
    finally:
        sys.path.pop(0)
    return [(s["start"], s["end"]) for s in mod.parse_srt(path)]


def check_subtitles(path: str | None, duration: float, spec: dict) -> Check:
    if not path or not os.path.isfile(path):
        return Check("subtitles", SKIP, "字幕ファイルなし")
    cues = _parse_cues(path)
    if not cues:
        return Check("subtitles", FAIL, f"字幕を 1 件も読めません({os.path.basename(path)})")
    tol = spec.get("durationToleranceSec", 0.10)
    problems = []
    for i, (s, e) in enumerate(cues):
        if e <= s:
            problems.append(f"#{i + 1} の終了が開始より前({s:.2f}→{e:.2f})")
        if i and s + 1e-6 < cues[i - 1][0]:
            problems.append(f"#{i + 1} の開始が前の字幕より前")
        if e > duration + tol:
            problems.append(f"#{i + 1} の終了 {e:.2f} 秒が動画の長さ {duration:.2f} 秒を超える")
    if problems:
        return Check("subtitles", FAIL, "; ".join(problems[:4]), cues=len(cues))
    return Check("subtitles", PASS, f"{len(cues)} 件が時刻順・動画の長さの内側", cues=len(cues))


def check_chapters(meta: dict | None, duration: float, spec: dict) -> Check:
    chapters = (meta or {}).get("chapters")
    if not chapters:
        return Check("chapters", SKIP, "章(chapters)の記録なし(古い書き出し)")
    tol = spec.get("chapterGapToleranceSec", 0.15)
    problems, t = [], 0.0
    for i, c in enumerate(chapters):
        start, dur = c.get("startSec", c.get("start_sec")), c.get("durationSec", c.get("duration_sec"))
        if start is None or dur is None or dur <= 0:
            problems.append(f"章 {i + 1} の時刻が不正")
            continue
        if abs(start - t) > tol:
            problems.append(f"章 {i + 1} の開始 {start:.2f} 秒が、前の章の終わり {t:.2f} 秒と合わない")
        t = start + dur
    if abs(t - duration) > max(tol, 0.5):
        problems.append(f"章の合計 {t:.2f} 秒が動画の長さ {duration:.2f} 秒と合わない")
    if problems:
        return Check("chapters", FAIL, "; ".join(problems[:4]))
    return Check("chapters", PASS, f"{len(chapters)} 章が途切れなく動画の長さに一致")


# ----------------------------------------------------------------------------- コンタクトシート・決定性
def scene_times(path: str, threshold: float = 0.10, limit: int = 16) -> list[float]:
    # 0.30 では、同じ構図のスライドが続く動画で 1 件も検出できなかった(実測)。0.10 で 8 件。新しい書き出しは chapters を持つので、そちらが基準になる。
    out = _run(["ffmpeg", "-v", "info", "-i", path, "-an", "-vf", f"select='gt(scene,{threshold})',showinfo", "-f", "null", "-"])
    times = [float(x) for x in re.findall(r"pts_time:([\d.]+)", out.stderr)]
    if len(times) > limit:
        step = len(times) / limit
        times = [times[int(i * step)] for i in range(limit)]
    return times


def contact_sheet(path: str, duration: float, meta: dict | None, out_png: str, offset: float = 0.25, thumb_w: int = 320, max_beats: int = 16) -> Check:
    chapters = (meta or {}).get("chapters")
    if chapters:
        beats = [float(c.get("startSec", c.get("start_sec", 0))) for c in chapters]
        if len(beats) > max_beats:
            beats = [beats[int(i * len(beats) / max_beats)] for i in range(max_beats)]
        source = "章の開始"
    else:
        beats = scene_times(path)
        source = "場面の切り替わり(ffmpeg の検出)"
    if not beats:
        beats = [duration * (i + 0.5) / 8 for i in range(8)]
        source = "等間隔(切り替わりを検出できなかった)"
    thumb_h = int(thumb_w * 9 / 16)
    label_h, pad = 18, 6
    cols = 3
    sheet = Image.new("RGB", (cols * thumb_w + (cols + 1) * pad, len(beats) * (thumb_h + label_h + pad) + pad), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    for r, b in enumerate(beats):
        y = pad + r * (thumb_h + label_h + pad)
        draw.text((pad, y), f"beat {r + 1}  t={b:.2f}s   before / beat / after (+-{offset:.2f}s)", fill=(200, 200, 200))  # 画像内は英語(PIL の既定フォントに日本語が無い)
        for c, t in enumerate((b - offset, b, b + offset)):
            im = frame_at(path, min(max(t, 0), max(duration - 0.1, 0))).resize((thumb_w, thumb_h))
            sheet.paste(im, (pad + c * (thumb_w + pad), y + label_h))
    os.makedirs(os.path.dirname(os.path.abspath(out_png)), exist_ok=True)
    sheet.save(out_png)
    return Check("contact-sheet", PASS, f"{len(beats)} 拍(基準: {source})→ {out_png}。**人が見て判断する**(合否は付けない)", beats=len(beats))


def check_twin(a: str, b: str, duration: float, spec: dict, samples: int = 8) -> Check:
    limit = spec.get("twinMeanAbsDiff", 1.5)
    other = probe(b)["video_duration"] or probe(b)["format_duration"]
    if abs(other - duration) > spec.get("durationToleranceSec", 0.10):
        return Check("determinism", FAIL, f"長さが違う({duration:.3f} 秒と {other:.3f} 秒)。同じ入力の書き出しではない")
    worst, bad = 0.0, []
    for i in range(samples):
        t = duration * (i + 0.5) / samples
        fa, fb = frame_at(a, t).resize((160, 90)), frame_at(b, t).resize((160, 90))
        pa, pb = fa.getdata(), fb.getdata()
        mad = sum(abs(x - y) for p, q in zip(pa, pb) for x, y in zip(p, q)) / (len(pa) * 3)
        worst = max(worst, mad)
        if mad > limit:
            bad.append(f"{t:.1f}s(平均差 {mad:.2f})")
    if bad:
        return Check("determinism", FAIL, "同じ入力なのに、フレームが違う: " + ", ".join(bad), worst=worst)
    return Check("determinism", PASS, f"{samples} 点すべて同じ(最大の平均差 {worst:.2f} / 255。許容 {limit})", worst=worst)


# ----------------------------------------------------------------------------- 全体
def resolve_target(target: str) -> tuple[str, str | None, str | None]:
    """(動画, metadata.json, 字幕) を返す。"""
    if os.path.isdir(target):
        video = os.path.join(target, "video.mp4")
        meta = os.path.join(target, "metadata.json")
        subs = next((os.path.join(target, n) for n in ("subtitles.vtt", "subtitles.srt") if os.path.isfile(os.path.join(target, n))), None)
        return video, meta if os.path.isfile(meta) else None, subs
    base = os.path.splitext(target)[0]
    subs = next((base + ext for ext in (".vtt", ".srt") if os.path.isfile(base + ext)), None)
    meta = os.path.join(os.path.dirname(target), "metadata.json")
    return target, meta if os.path.isfile(meta) else None, subs


def verify(target: str, spec: dict, subtitles: str | None = None, contact: str | None = None, twin: str | None = None) -> list[Check]:
    video, meta_path, subs = resolve_target(target)
    if not os.path.isfile(video):
        return [Check("input", FAIL, f"動画がありません: {video}")]
    meta = None
    if meta_path:
        try:
            meta = json.load(open(meta_path, encoding="utf-8"))
        except (OSError, ValueError):
            meta = None
    try:
        info = probe(video)
    except RuntimeError as err:
        return [Check("input", FAIL, str(err))]
    duration = info["video_duration"] or info["format_duration"]
    checks = [check_streams(info, spec), check_av_duration(info, spec)]
    if info["video"]:
        checks.append(check_colors(video, duration, spec))
        checks.append(check_black(video, duration, spec))
    if info["audio"]:
        checks.append(check_audio_levels(video, spec))
    checks.append(check_subtitles(subtitles or subs, duration, spec))
    checks.append(check_chapters(meta, duration, spec))
    if twin:
        checks.append(check_twin(video, twin, duration, spec))
    if contact and info["video"]:
        checks.append(contact_sheet(video, duration, meta, contact))
    return checks


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="書き出した動画の検査(音声と映像の長さ・配色・黒フレーム・音割れ・字幕・章・コンタクトシート)")
    ap.add_argument("target", help="動画(.mp4)・動画のフォルダ(video.mp4 と metadata.json がある)・--all のときは、その親フォルダ")
    ap.add_argument("--spec", default=DEFAULT_SPEC, help="検査の基準(既定: tools/video_spec.json)")
    ap.add_argument("--subtitles", help="字幕ファイル(.srt / .vtt)。省略時は動画の隣を探す")
    ap.add_argument("--contact-sheet", metavar="PNG", help="コンタクトシート(拍ごとの前・拍・後)を書き出す")
    ap.add_argument("--twin", metavar="MP4", help="同じ入力を 2 回書き出した、もう 1 本。フレームが同じか調べる")
    ap.add_argument("--all", action="store_true", help="target の下の、video.mp4 があるフォルダをすべて検査する")
    ap.add_argument("--json", metavar="PATH", help="結果を JSON で書き出す")
    args = ap.parse_args(argv)
    for stream in (sys.stdout, sys.stderr):  # Windows の既定(cp932)だと日本語が化ける
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    need_tools()
    spec = json.load(open(args.spec, encoding="utf-8"))

    targets = [args.target]
    if args.all:
        targets = sorted(os.path.join(args.target, d) for d in os.listdir(args.target) if os.path.isfile(os.path.join(args.target, d, "video.mp4")))
        if not targets:
            print(f"{args.target} の下に video.mp4 がありません", file=sys.stderr)
            return 2

    report, failed = {}, 0
    icon = {PASS: "ok  ", WARN: "warn", FAIL: "FAIL", SKIP: "skip"}
    for t in targets:
        checks = verify(t, spec, args.subtitles, args.contact_sheet if len(targets) == 1 else None, args.twin if len(targets) == 1 else None)
        report[t] = [c.as_dict() for c in checks]
        bad = [c for c in checks if c.status == FAIL]
        failed += bool(bad)
        print(f"\n== {t}  {'不合格' if bad else '合格'}")
        for c in checks:
            if args.all and c.status in (PASS, SKIP):
                continue
            print(f"  [{icon[c.status]}] {c.name}: {c.detail}")
    if args.all:
        print(f"\n{len(targets)} 本中 {failed} 本が不合格")
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, ensure_ascii=False, indent=2)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
