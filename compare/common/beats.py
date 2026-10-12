"""ビート検出(V3): 音を周波数に分解して立ち上がりを検出し、周期から BPM を割り出す。

librosa 等には依存せず numpy だけで実装(RAGReel の比較実装は依存を増やさない方針)。
手順: STFT → スペクトルフラックス(各周波数の増加分の合計)= 発音の立ち上がり →
自己相関で周期(BPM) → 位相を探して拍の時刻列を作る。
"""
from __future__ import annotations

import wave

import numpy as np


def read_wav_mono(path: str):
    with wave.open(path, "rb") as w:
        sr, ch, sw = w.getframerate(), w.getnchannels(), w.getsampwidth()
        raw = w.readframes(w.getnframes())
    if sw != 2:
        raise ValueError("16bit PCM only")
    x = np.frombuffer(raw, dtype=np.int16).astype(np.float64) / 32768.0
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    return x, sr


def onset_envelope(x: np.ndarray, sr: int, n_fft: int = 1024, hop: int = 256):
    win = np.hanning(n_fft)
    n = 1 + max(0, (len(x) - n_fft) // hop)
    spec = np.empty((n, n_fft // 2 + 1))
    for i in range(n):
        spec[i] = np.abs(np.fft.rfft(x[i * hop:i * hop + n_fft] * win))
    logspec = np.log1p(10.0 * spec)
    flux = np.maximum(0.0, np.diff(logspec, axis=0)).sum(axis=1)
    flux = np.concatenate([[0.0], flux])
    flux = flux - np.convolve(flux, np.ones(21) / 21, mode="same")  # 局所平均を引く
    flux = np.maximum(flux, 0.0)
    if flux.max() > 0:
        flux /= flux.max()
    return flux, sr / hop  # 包絡, 包絡のサンプリングレート(Hz)


def estimate_tempo(env: np.ndarray, env_rate: float, bpm_min: float = 60.0, bpm_max: float = 180.0):
    e = env - env.mean()
    ac = np.correlate(e, e, mode="full")[len(e) - 1:]
    lo, hi = int(env_rate * 60.0 / bpm_max), int(env_rate * 60.0 / bpm_min)
    hi = min(hi, len(ac) - 1)
    if hi <= lo:
        return 0.0, 0
    # 倍テンポ/半テンポの取り違えを減らすため、整数倍ラグの寄与も足す
    score = np.zeros(hi + 1)
    for lag in range(lo, hi + 1):
        s = ac[lag]
        if 2 * lag < len(ac):
            s += 0.5 * ac[2 * lag]
        score[lag] = s
    lag = int(np.argmax(score[lo:hi + 1])) + lo
    # 放物線補間でサブフレーム精度に
    if 1 <= lag < len(ac) - 1:
        y0, y1, y2 = ac[lag - 1], ac[lag], ac[lag + 1]
        den = (y0 - 2 * y1 + y2)
        shift = 0.5 * (y0 - y2) / den if den != 0 else 0.0
    else:
        shift = 0.0
    period = (lag + shift) / env_rate
    return 60.0 / period, lag


def beat_times(env: np.ndarray, env_rate: float, bpm: float, duration: float) -> list:
    period = 60.0 / bpm
    best_phase, best_score = 0.0, -1.0
    for ph in np.arange(0.0, period, 1.0 / env_rate):
        ts = np.arange(ph, duration, period)
        idx = np.clip(np.round(ts * env_rate).astype(int), 0, len(env) - 1)
        sc = env[idx].sum()
        if sc > best_score:
            best_score, best_phase = sc, ph
    return [round(float(t), 3) for t in np.arange(best_phase, duration, period)]


def detect(path: str) -> dict:
    x, sr = read_wav_mono(path)
    env, rate = onset_envelope(x, sr)
    bpm, _ = estimate_tempo(env, rate)
    dur = len(x) / sr
    return {"bpm": round(bpm, 2), "duration": round(dur, 3), "beats": beat_times(env, rate, bpm, dur) if bpm else []}


def snap_to_beats(times: list, beats: list, max_shift: float = 0.6) -> list:
    """場面転換の時刻を最も近い拍に寄せる(最大 max_shift 秒まで。超える場合は元の時刻)。"""
    if not beats:
        return list(times)
    out = []
    for t in times:
        b = min(beats, key=lambda v: abs(v - t))
        out.append(b if abs(b - t) <= max_shift else t)
    return out


def synth_click_track(path: str, bpm: float, seconds: float = 20.0, sr: int = 44100, seed: int = 7):
    """検証用の合成BGM: 拍ごとにキック+裏拍にハイハット+定常ノイズ。BPMが既知。"""
    rng = np.random.default_rng(seed)
    n = int(seconds * sr)
    x = rng.normal(0, 0.01, n)
    period = 60.0 / bpm
    t = 0.0
    k = 0
    while t < seconds - 0.2:
        i = int(t * sr)
        m = int(0.12 * sr)
        tt = np.arange(m) / sr
        kick = np.sin(2 * np.pi * (110 * np.exp(-tt * 18)) * tt) * np.exp(-tt * 22) * 0.9
        x[i:i + m] += kick[:max(0, min(m, n - i))]
        h = int(t * sr + period * 0.5 * sr)
        mm = int(0.03 * sr)
        if h + mm < n:
            x[h:h + mm] += rng.normal(0, 0.25, mm) * np.exp(-np.arange(mm) / (0.008 * sr))
        t += period
        k += 1
    x = np.clip(x, -1, 1)
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
        w.writeframes((x * 32767).astype(np.int16).tobytes())
    return path
