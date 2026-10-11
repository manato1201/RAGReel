#!/usr/bin/env python3
"""VOICEVOX の声を聴き比べるための WAV を作る(標準ライブラリのみ)。

使い方:
  1. VOICEVOX ENGINE を起動する(docs/narration-quality.md の手順)
  2. python tools/tts_ab_test.py                # 既定: 春日部つむぎ(ノーマル)
     python tools/tts_ab_test.py --all          # 全話者の ノーマル
     python tools/tts_ab_test.py --names 春日部つむぎ 四国めたん --speed 1.05
  3. tts_samples/ に、声 × 文 の WAV ができる。聴いて選ぶ。

文は RAGReel のナレーションで実際に出てくる種類(専門用語・数字・長めの説明)にしてある。
SAPI(Microsoft Haruka)との比較用に、既存の NarrationEngine と同じ 44.1kHz/16bit/mono で書き出す。
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import urllib.parse
import urllib.request
import wave
from pathlib import Path

SAMPLES = {
    "term": "Houdini の Niagara ROP で書き出した点群を、Unreal Engine の Niagara で再生します。",
    "number": "粒子は四万八千個、六十フレームで、ファイルサイズは約百二十メガバイトです。",
    "long": "まず、メッシュの単位を確認します。Houdini は一メートル、Unreal は一センチを一単位として扱うため、"
            "取り込み時にスケールを百倍にする必要があります。次に、属性の名前を揃えます。",
}


def http(method: str, url: str, body: bytes | None = None, timeout: int = 30) -> bytes:
    req = urllib.request.Request(url, data=body, method=method)
    if body is not None:
        req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def to_wav_44k(raw: bytes, out: Path) -> float:
    """VOICEVOX の WAV(通常 24kHz)を 44.1kHz mono 16bit に直して書く。長さ(秒)を返す。"""
    import io
    with wave.open(io.BytesIO(raw)) as w:
        rate, ch, width, n = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
        data = w.readframes(n)
    if width != 2:
        raise RuntimeError(f"unsupported sample width: {width}")
    samples = list(struct.unpack("<" + "h" * (len(data) // 2), data))
    if ch == 2:
        samples = [(samples[i] + samples[i + 1]) // 2 for i in range(0, len(samples) - 1, 2)]
    target = 44100
    if rate != target:
        ratio = target / rate
        m = int(round(len(samples) * ratio))
        res = []
        for i in range(m):
            src = i / ratio
            i0 = int(src)
            i1 = min(i0 + 1, len(samples) - 1)
            f = src - i0
            res.append(int(round(samples[i0] * (1 - f) + samples[i1] * f)))
        samples = res
    with wave.open(str(out), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(target)
        w.writeframes(struct.pack("<" + "h" * len(samples), *samples))
    return len(samples) / target


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default="http://127.0.0.1:50021")
    ap.add_argument("--names", nargs="*", default=["春日部つむぎ"], help="話者名(部分一致)。既定: 春日部つむぎ。全員を聴くときは --names '' を渡す代わりに --all")
    ap.add_argument("--all", action="store_true", help="全話者の『ノーマル』を作る")
    ap.add_argument("--style", default="ノーマル")
    ap.add_argument("--speed", type=float, default=1.0)
    ap.add_argument("--intonation", type=float, default=1.0)
    ap.add_argument("--out", default="tts_samples")
    args = ap.parse_args()

    try:
        version = http("GET", f"{args.url}/version").decode("utf-8")
    except Exception as e:  # noqa: BLE001
        print(f"VOICEVOX ENGINE に接続できません ({args.url}): {e}", file=sys.stderr)
        print("docs/narration-quality.md の『エンジンの起動』を確認してください。", file=sys.stderr)
        return 2
    print(f"engine version: {version}")

    speakers = json.loads(http("GET", f"{args.url}/speakers"))
    picks = []
    for sp in speakers:
        if not args.all and args.names and not any(n in sp["name"] for n in args.names):
            continue
        for st in sp["styles"]:
            if st["name"] == args.style:
                picks.append((sp["name"], st["id"]))
    if not picks:
        print("該当する話者がありません", file=sys.stderr)
        return 3

    out_dir = Path(args.out)
    out_dir.mkdir(exist_ok=True)
    for name, sid in picks:
        for key, text in SAMPLES.items():
            q = http("POST", f"{args.url}/audio_query?" + urllib.parse.urlencode({"text": text, "speaker": sid}))
            query = json.loads(q)
            query.update(speedScale=args.speed, intonationScale=args.intonation, outputStereo=False)
            wav = http("POST", f"{args.url}/synthesis?speaker={sid}", json.dumps(query).encode("utf-8"))
            path = out_dir / f"{name}_{sid}_{key}.wav"
            dur = to_wav_44k(wav, path)
            print(f"{path}  {dur:.1f}s")
    print("\n聴いたら、使う声を RAGREEL_VOICEVOX_SPEAKER に設定してください。クレジット: VOICEVOX:<キャラクター名>")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
