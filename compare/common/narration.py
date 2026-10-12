"""ナレーション合成と「読み上げの時刻情報」(V7)。

文単位で合成してWAVを連結するため、各文の開始・終了時刻は推定ではなく
連結したサンプル数から正確に分かる(Qt版 VoicevoxNarrationEngine と同じ方式)。
この PC では VOICEVOX が未導入なので、既定は Windows SAPI5(Haruka)。
"""
from __future__ import annotations

import json
import re
import subprocess
import wave

import numpy as np
from dataclasses import dataclass, asdict
from pathlib import Path

SAMPLE_RATE = 44100
_SPLIT = re.compile(r"(?<=[。！？!?])\s*|\n+")


def split_sentences(text: str, max_chars: int = 60) -> list:
    out = []
    for part in _SPLIT.split(text):
        part = part.strip()
        if not part:
            continue
        while len(part) > max_chars:  # 長文は読点で切る(無ければ強制)
            cut = part.rfind("、", 0, max_chars)
            cut = cut + 1 if cut > 8 else max_chars
            out.append(part[:cut].strip())
            part = part[cut:].strip()
        if part:
            out.append(part)
    return out


@dataclass
class Segment:
    scene: int
    text: str
    start: float
    end: float


_PS = r'''
param([string]$InJson, [string]$OutDir)
Add-Type -AssemblyName System.Speech
$items = Get-Content -Raw -Encoding UTF8 $InJson | ConvertFrom-Json
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
$ja = $s.GetInstalledVoices() | Where-Object { $_.VoiceInfo.Culture.Name -eq 'ja-JP' } | Select-Object -First 1
if ($ja) { $s.SelectVoice($ja.VoiceInfo.Name) }
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(44100, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
$i = 0
foreach ($t in $items) {
  $p = Join-Path $OutDir ("s{0:D4}.wav" -f $i)
  $s.SetOutputToWaveFile($p, $fmt)
  $s.Speak([string]$t)
  $s.SetOutputToNull()
  $i++
}
$s.Dispose()
'''


def _read_pcm(path: Path) -> bytes:
    with wave.open(str(path), "rb") as w:
        assert w.getnchannels() == 1 and w.getsampwidth() == 2, path
        if w.getframerate() != SAMPLE_RATE:
            raise RuntimeError(f"unexpected sample rate {w.getframerate()}")
        return w.readframes(w.getnframes())


def _trim_silence(raw: bytes, thresh_db: float = -45.0, margin: float = 0.03) -> bytes:
    """文頭・文末のTTS由来の無音を削る。字幕の開始時刻を「実際の発話開始」に一致させるため。"""
    x = np.frombuffer(raw, dtype=np.int16)
    if x.size == 0:
        return raw
    lim = 32768.0 * (10 ** (thresh_db / 20))
    idx = np.nonzero(np.abs(x) > lim)[0]
    if idx.size == 0:
        return raw
    m = int(margin * SAMPLE_RATE)
    a, b = max(0, idx[0] - m), min(x.size, idx[-1] + 1 + m)
    return x[a:b].tobytes()


def synthesize(scenes: list, outdir: str, sentence_gap: float = 0.25, scene_gap: float = 0.5) -> dict:
    """scenes: [{'narration': str}, ...]. 戻り値: wav / segments / scene_ranges / duration。"""
    out = Path(outdir)
    out.mkdir(parents=True, exist_ok=True)
    sentences = []  # (scene_index, text)
    for si, sc in enumerate(scenes):
        for t in split_sentences(sc.get("narration", "")):
            sentences.append((si, t))
    if not sentences:
        raise RuntimeError("nothing to synthesize")

    in_json = out / "sentences.json"
    in_json.write_text(json.dumps([t for _, t in sentences], ensure_ascii=False), encoding="utf-8")
    ps1 = out / "_tts.ps1"
    ps1.write_text(_PS, encoding="utf-8-sig")
    subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(ps1),
                    "-InJson", str(in_json), "-OutDir", str(out)], check=True, capture_output=True)

    pcm = bytearray()
    segments: list[Segment] = []
    scene_ranges: dict = {}
    pos = 0.0
    prev_scene = None
    for i, (si, text) in enumerate(sentences):
        gap = 0.0 if prev_scene is None else (scene_gap if si != prev_scene else sentence_gap)
        if gap:
            pcm += b"\x00\x00" * int(round(gap * SAMPLE_RATE))
            pos += gap
        raw = _trim_silence(_read_pcm(out / f"s{i:04d}.wav"))
        dur = len(raw) / 2 / SAMPLE_RATE
        segments.append(Segment(si, text, round(pos, 3), round(pos + dur, 3)))
        pcm += raw
        pos += dur
        r = scene_ranges.setdefault(si, [segments[-1].start, segments[-1].end])
        r[1] = segments[-1].end
        prev_scene = si

    wav_path = out / "narration.wav"
    with wave.open(str(wav_path), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SAMPLE_RATE)
        w.writeframes(bytes(pcm))
    result = {"wav": str(wav_path), "duration": round(pos, 3),
              "segments": [asdict(s) for s in segments],
              "scene_ranges": {str(k): v for k, v in scene_ranges.items()}}
    (out / "timings.json").write_text(json.dumps(result, ensure_ascii=False, indent=1), encoding="utf-8")
    return result
