"""V2 CLI: チュートリアルmd + スクリーンショットmanifest → MP4(Pythonのみ)。

  python -m python_render.render --md X.md --shots X_screenshots.json --out out/python.mp4

読み上げの時刻情報(common/narration.py)で場面の切替と字幕を決める。BGMがあれば拍に
場面転換を寄せる(ただし転換は「前の場面の発話終了 〜 次の発話開始」の間にだけ寄せ、
映像と音声の対応は崩さない)。
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
import wave

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from common import beats as beatlib, narration, subtitles  # noqa: E402
from common.tutorial_input import load_scenes  # noqa: E402
from python_render import engine  # noqa: E402


def plan_boundaries(ranges: dict, n_scenes: int, beat_list: list) -> list:
    """各場面の表示開始時刻。境界は発話の隙間(prev_end..next_start)に収め、拍があれば最寄りの拍。"""
    starts = [0.0]
    for i in range(1, n_scenes):
        prev_end = ranges[str(i - 1)][1]
        nxt = ranges[str(i)][0]
        mid = (prev_end + nxt) / 2
        t = mid
        if beat_list:
            cands = [b for b in beat_list if prev_end <= b <= nxt]
            if cands:
                t = min(cands, key=lambda b: abs(b - mid))
        starts.append(t)
    return starts


def mix_bgm(narr_wav: str, bgm_wav: str, out_wav: str, gain: float = 0.22):
    def rd(p):
        with wave.open(p, "rb") as w:
            return np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32), w.getframerate()
    a, sr = rd(narr_wav)
    b, sr2 = rd(bgm_wav)
    assert sr == sr2
    if len(b) < len(a):
        b = np.tile(b, int(np.ceil(len(a) / len(b))))
    mix = np.clip(a + gain * b[: len(a)], -32768, 32767).astype(np.int16)
    with wave.open(out_wav, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
        w.writeframes(mix.tobytes())


def render(md, shots, out_mp4, width=1280, height=720, fps=30, bgm_bpm=None, workdir=None) -> dict:
    t0 = time.time()
    work = workdir or os.path.join(os.path.dirname(out_mp4) or ".", "py_work")
    os.makedirs(work, exist_ok=True)
    data = load_scenes(md, shots)
    scenes = data["scenes"]
    tn = time.time()
    narr = narration.synthesize(scenes, os.path.join(work, "narr"))
    t_narr = time.time() - tn

    audio, beat_list = narr["wav"], []
    if bgm_bpm:
        bgm = beatlib.synth_click_track(os.path.join(work, "bgm.wav"), bgm_bpm, seconds=narr["duration"] + 4)
        beat_list = beatlib.detect(bgm)["beats"]
        audio = os.path.join(work, "mixed.wav")
        mix_bgm(narr["wav"], bgm, audio)

    starts = plan_boundaries(narr["scene_ranges"], len(scenes), beat_list)
    total = narr["duration"] + 1.5
    ends = starts[1:] + [total]
    objs = [engine.Scene(sc, width, height) for sc in scenes]
    n_steps = sum(1 for s in scenes if s["kind"] == "step")
    segs = narr["segments"]

    enc = engine.open_encoder(out_mp4, width, height, fps, audio)
    n_frames = int(round(total * fps))
    seg_i = 0
    half = engine.TRANSITION_SEC / 2
    for fi in range(n_frames):
        t = fi / fps
        idx = max(i for i, s in enumerate(starts) if s <= t)
        completed = sum(1 for s in scenes[:idx] if s["kind"] == "step")
        frame = objs[idx].render(t - starts[idx], completed, n_steps)
        if idx + 1 < len(objs) and t >= ends[idx] - half:  # 次の場面への遷移(境界の前後 TRANSITION_SEC/2)
            tb = ends[idx]
            comp2 = completed + (1 if scenes[idx]["kind"] == "step" else 0)
            f2 = objs[idx + 1].render(max(0.0, t - tb), comp2, n_steps)
            kind = engine.CHAPTER_TRANSITION.get(scenes[idx + 1]["chapter"], "fade")
            frame = engine.transition(kind, frame, f2, (t - (tb - half)) / engine.TRANSITION_SEC)
        frame = engine.apply_vignette(frame)
        while seg_i + 1 < len(segs) and segs[seg_i]["end"] < t:
            seg_i += 1
        cur = segs[seg_i]
        engine.draw_subtitle(frame, cur["text"] if cur["start"] - 0.05 <= t <= cur["end"] + 0.15 else "",
                             width / 1280.0)
        enc.stdin.write(frame.tobytes())
    enc.stdin.close()
    enc.wait()

    srt = os.path.splitext(out_mp4)[0] + ".srt"
    with open(srt, "w", encoding="utf-8") as f:
        f.write(subtitles.to_srt(segs))
    info = {"impl": "python", "out": out_mp4, "srt": srt, "seconds": round(time.time() - t0, 1),
            "narration_seconds": round(t_narr, 1), "duration": round(total, 2), "frames": n_frames,
            "size_bytes": os.path.getsize(out_mp4), "width": width, "height": height, "fps": fps,
            "scenes": len(scenes), "beats_used": bool(beat_list), "bgm_bpm": bgm_bpm,
            "transitions": engine.CHAPTER_TRANSITION}
    with open(os.path.splitext(out_mp4)[0] + ".json", "w", encoding="utf-8") as f:
        json.dump(info, f, ensure_ascii=False, indent=1)
    return info


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--md", required=True)
    ap.add_argument("--shots")
    ap.add_argument("--out", required=True)
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--bgm-bpm", type=float, default=None, help="検証用の合成BGMを重ね、拍に転換を寄せる")
    a = ap.parse_args()
    print(json.dumps(render(a.md, a.shots, a.out, a.width, a.height, a.fps, a.bgm_bpm), ensure_ascii=False, indent=1))
