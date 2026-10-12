"""比較結果を web/public/compare/ に書き出す(compare.html が読む)。

  out/metrics.json + out/v6_eval.json + 既知BPM検証 + out/*.mp4 → web/public/compare/{data.js, *.mp4, *.webm, *.srt}
動画ファイルは .gitignore 対象(*.mp4 / *.webm)。デプロイ時にローカルにあるものだけが配信される。
"""
from __future__ import annotations

import datetime
import glob
import json
import os
import re
import shutil
import sys
import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(ROOT)
sys.path.insert(0, ROOT)
from common import beats as beatlib  # noqa: E402
from python_render import side_by_side  # noqa: E402

OUT = os.path.join(ROOT, "out")
PUB = os.path.join(REPO, "web", "public", "compare")


def parse_hf_stages(log_path: str) -> dict:
    if not os.path.exists(log_path):
        return {}
    text = open(log_path, encoding="utf-8", errors="ignore").read()
    m = re.search(r"compile ([\dms. ]+) · .*?capture ([\dms. ]+) · .*?assemble ([\dms. ]+)", text)

    def secs(s):
        mm = re.search(r"(?:(\d+)m)?\s*([\d.]+)s", s)
        return (int(mm.group(1) or 0) * 60 + float(mm.group(2))) if mm else 0.0
    return {"compile": secs(m.group(1)), "capture": secs(m.group(2)), "assemble": secs(m.group(3))} if m else {}


def qt_stages(log_path: str) -> dict:
    if not os.path.exists(log_path):
        return {}
    return {k: float(v) for k, v in re.findall(r"Stage (\w+): ok \(([\d.]+)s\)", open(log_path, encoding="utf-8", errors="ignore").read())}


def main():
    metrics = json.load(open(os.path.join(OUT, "metrics.json"), encoding="utf-8"))
    v6 = json.load(open(os.path.join(OUT, "v6_eval.json"), encoding="utf-8"))
    os.makedirs(PUB, exist_ok=True)

    # V3: 既知BPM
    beat_rows = []
    with tempfile.TemporaryDirectory() as d:
        for bpm in (90, 100, 120, 128, 140, 150):
            p = beatlib.synth_click_track(os.path.join(d, "t.wav"), bpm, seconds=24)
            r = beatlib.detect(p)
            gaps = [b - a for a, b in zip(r["beats"], r["beats"][1:])]
            beat_rows.append({"bpm": bpm, "detected": r["bpm"],
                              "max_gap_err": round(max(abs(g - 60.0 / bpm) for g in gaps), 4)})

    # 動画のコピー
    videos = {}
    for key, src, dst in (("Python", "python.mp4", "python.mp4"), ("HyperFrames", "hyperframes.mp4", "hyperframes.mp4")):
        s = os.path.join(OUT, src)
        if os.path.exists(s):
            shutil.copy(s, os.path.join(PUB, dst))
            videos[key] = "compare/" + dst
    qt = sorted(glob.glob(os.path.join(OUT, "qt_run", "*.webm")))
    if qt:
        shutil.copy(qt[-1], os.path.join(PUB, "qt.webm"))
        videos["Qt"] = "compare/qt.webm"
    if os.path.exists(os.path.join(OUT, "python.srt")):
        shutil.copy(os.path.join(OUT, "python.srt"), os.path.join(PUB, "subtitles.srt"))

    # V5: 3方式を段階表示する解説(同じテンプレート)
    qs = qt_stages(os.path.join(OUT, "qt_run", "qt.log"))
    hs = parse_hf_stages(os.path.join(OUT, "hf_run.log"))
    py = json.load(open(os.path.join(OUT, "python.json"), encoding="utf-8"))
    spec = {
        "title": "同じ入力を3つの方式で動画にする",
        "input": "チャーシュー(10ステップ)の手順書とスクリーンショット",
        "methods": [
            {"name": "Qt/QML", "steps": [["取り込み", qs.get("ingest", 0)], ["構成", qs.get("compose", 0)],
                                         ["ナレーション", qs.get("narrate", 0)], ["描画+エンコード", qs.get("render", 0)],
                                         ["公開", qs.get("publish", 0)]]},
            {"name": "HyperFrames", "steps": [["取り込み", 0], ["HTML生成", 0], ["ブラウザ描画", hs.get("capture", 0)],
                                              ["結合", hs.get("assemble", 0)]]},
            {"name": "Python", "steps": [["取り込み", 0], ["ナレーション", py.get("narration_seconds", 0)],
                                         ["場面計画", 0], ["フレーム合成", py.get("seconds", 0)], ["書き出し", 0]]},
        ],
        "metric": {"label": "生成時間(秒)", "values": {
            "Qt/QML": round(metrics.get("Qt", {}).get("seconds") or 0, 1),
            "HyperFrames": round(metrics.get("HyperFrames", {}).get("seconds") or 0, 1),
            "Python": round(metrics.get("Python", {}).get("seconds") or 0, 1)}},
    }
    side_by_side.render(spec, os.path.join(PUB, "side_by_side.mp4"))

    labels = {"hash/text": "文字2-gramハッシュ × 説明文", "st/text": "多言語テキストモデル × 説明文",
              "st/image": "CLIP画像埋め込み × 画像そのもの", "st/auto": "説明文 + 画像(高い方)"}
    data = {
        "generated_at": datetime.date.today().isoformat(),
        "input": {"title": "ぷるとろチャーシュー風の塊肉を作る", "steps": 10},
        "metrics": metrics,
        "videos": videos,
        "side_by_side": "compare/side_by_side.mp4",
        "search": {"labels": labels, "results": {k: {"top1": v["top1"], "top3": v["top3"], "n": v["n"]} for k, v in v6.items()}},
        "beats": beat_rows,
        "notes": {
            "metrics": "同じ入力でも出力の長さは揃っていません(Qt版は手順書の全章を読み上げるため約3分、"
                       "Python/HyperFrames版は概要・手順・ハマりポイント・ヒントのみで約2分)。そのため"
                       "「出力1秒あたりの生成時間」を併記しています。Qt版の字幕は音声合成がSAPIのため概算です"
                       "(VOICEVOXでは実時刻になります)。生成時間はPC構成や負荷で数割動きます(Qt版は1回、HyperFrames・Python版は2回計測した最後の値)。",
            "search": "10問の言い換えクエリでの正解率(正解=そのステップの画像)。サンプルが小さいため目安です。"
                      "Houdiniのネットワーク画面のようなUIスクリーンショットは、画像そのものの埋め込みでは"
                      "区別がつきにくく(偶然の3倍弱)、説明文との照合のほうが当たります。画像検索は補助にとどめ、"
                      "説明文を索引の主とする方針です。EmbeddingGemma 2は未検証です。",
        },
    }
    with open(os.path.join(PUB, "data.js"), "w", encoding="utf-8") as f:
        f.write("window.__VF_COMPARE__ = " + json.dumps(data, ensure_ascii=False) + ";\n")
    print(json.dumps({"videos": videos, "out": PUB}, ensure_ascii=False))


if __name__ == "__main__":
    main()
