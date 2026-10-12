"""字幕(SRT / WebVTT)を、読み上げの時刻情報から生成する(V7)。

時刻は narration.synthesize() が文ごとに正確に出したもの。画面側(レンダラ)が
独自に「秒数を見積もる」ことはしない。ずれの測定(measure_offset)は
音声そのものから無音区間を検出し、字幕の文頭時刻と突き合わせる。
"""
from __future__ import annotations

import wave
from typing import Iterable

import numpy as np


def _ts(sec: float, vtt: bool = False) -> str:
    ms = int(round(sec * 1000))
    h, ms = divmod(ms, 3600000)
    m, ms = divmod(ms, 60000)
    s, ms = divmod(ms, 1000)
    return f"{h:02d}:{m:02d}:{s:02d}{'.' if vtt else ','}{ms:03d}"


def wrap_cjk(text: str, width: int = 22) -> list:
    """日本語字幕を width 文字ごとに折る(最大2行を想定)。"""
    return [text[i:i + width] for i in range(0, len(text), width)] or [""]


def to_srt(segments: Iterable[dict]) -> str:
    out = []
    for i, s in enumerate(segments, 1):
        out.append(f"{i}\n{_ts(s['start'])} --> {_ts(s['end'])}\n" + "\n".join(wrap_cjk(s['text'])) + "\n")
    return "\n".join(out)


def to_vtt(segments: Iterable[dict]) -> str:
    out = ["WEBVTT\n"]
    for s in segments:
        out.append(f"{_ts(s['start'], True)} --> {_ts(s['end'], True)}\n" + "\n".join(wrap_cjk(s['text'])) + "\n")
    return "\n".join(out)


def speech_onsets(wav_path: str, frame: float = 0.01, thresh_db: float = -40.0, min_silence: float = 0.18) -> list:
    """音声から「無音を挟んだ発話の開始時刻」を検出する(字幕ずれの実測用)。"""
    with wave.open(wav_path, "rb") as w:
        sr = w.getframerate()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64) / 32768.0
    hop = max(1, int(sr * frame))
    n = len(x) // hop
    rms = np.sqrt((x[:n * hop].reshape(n, hop) ** 2).mean(axis=1) + 1e-12)
    db = 20 * np.log10(rms + 1e-12)
    voiced = db > thresh_db
    onsets, silent_run = [], 10 ** 9
    for i, v in enumerate(voiced):
        if v:
            if silent_run * frame >= min_silence:
                onsets.append(round(i * frame, 3))
            silent_run = 0
        else:
            silent_run += 1
    return onsets


def measure_offset(segments: list, onsets: list, tolerance: float = 0.5) -> dict:
    """各字幕の開始時刻と、最も近い発話開始とのずれ(秒)。"""
    diffs = []
    for s in segments:
        if not onsets:
            break
        nearest = min(onsets, key=lambda o: abs(o - s["start"]))
        d = nearest - s["start"]
        if abs(d) <= tolerance:
            diffs.append(d)
    if not diffs:
        return {"n": 0, "mean_abs": None, "max_abs": None}
    a = np.abs(diffs)
    return {"n": len(diffs), "mean_abs": round(float(a.mean()), 3), "max_abs": round(float(a.max()), 3)}
