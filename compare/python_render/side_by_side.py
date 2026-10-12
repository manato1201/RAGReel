"""V5: 「同じ入力を複数の方式で並べる」解説テンプレート。

1つの計算(入力)を N 個の方式で段階表示する。ステップ番号と「完了数」を画面に出す
(例: CPU / GPU / NPU の動画のように)。入力は JSON:

  {
    "title": "同じ入力を3つの方式で動画にする",
    "input": "chashu-pork-block(10ステップ / 126秒)",
    "methods": [
      {"name": "Qt/QML", "steps": [["取り込み", 0.0], ["ナレーション", 0.5], ...], "result": "生成 Ns / 4.2MB"},
      ...
    ],
    "metric": {"label": "生成時間(秒)", "values": {"Qt/QML": 76, "HyperFrames": 204, "Python": 298}}
  }

各方式の steps は [名前, 所要秒] の列。列ごとに段階が順に点灯し、全方式が完了したら
指標のバーを出す。音声は付けない(無音のMP4)。
"""
from __future__ import annotations

import json
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from python_render import engine  # noqa: E402

COLORS = [(255, 138, 76), (96, 214, 168), (120, 170, 255), (240, 200, 90)]


def _rows_for(method: dict, total_rows: int):
    return method["steps"][:total_rows]


def render(spec: dict, out_mp4: str, width=1280, height=720, fps=30, step_sec=0.9) -> dict:
    methods = spec["methods"]
    n = len(methods)
    max_steps = max(len(m["steps"]) for m in methods)
    s = width / 1280.0
    intro, outro = 1.6, 4.0
    total = intro + max_steps * step_sec + 1.0 + outro
    n_frames = int(total * fps)

    title = engine.text_layer([spec["title"]], engine.font(engine.FONT_BOLD, int(46 * s)), engine.PALETTE["text"])
    sub = engine.text_layer([spec.get("input", "")], engine.font(engine.FONT_MED, int(24 * s)), engine.PALETTE["sub"])
    col_w = int(width * 0.28)
    gap = int((width - n * col_w) / (n + 1))
    heads = [engine.text_layer([m["name"]], engine.font(engine.FONT_BOLD, int(34 * s)), COLORS[i % 4]) for i, m in enumerate(methods)]
    stepfont = engine.font(engine.FONT_MED, int(22 * s))
    step_layers = [[engine.text_layer([f"{k + 1}. {name}"], stepfont, engine.PALETTE["text"])
                    for k, (name, _sec) in enumerate(m["steps"])] for m in methods]
    metric = spec.get("metric")

    enc = engine.open_encoder(out_mp4, width, height, fps, None)
    for fi in range(n_frames):
        t = fi / fps
        frame = engine.background(width, height).copy()
        engine.paste_rgba(frame, title, int(width * 0.06), int(height * 0.05))
        engine.paste_rgba(frame, sub, int(width * 0.06), int(height * 0.05) + int(70 * s))
        done_counts = []
        for i, m in enumerate(methods):
            x = gap + i * (col_w + gap)
            ty = int(height * 0.28)
            engine.paste_rgba(frame, heads[i], x, ty, alpha=min(1.0, t / 0.5))
            # 列の枠
            col = (np.array(COLORS[i % 4]) * 0.35).astype(np.uint8)
            frame[ty + int(54 * s):ty + int(58 * s), x:x + col_w] = col
            started = t - intro
            done = 0
            for k, layer in enumerate(step_layers[i]):
                at = k * step_sec
                if started < at:
                    break
                a = engine.ease_out_cubic((started - at) / 0.35)
                y = ty + int((74 + k * 34) * s)
                engine.paste_rgba(frame, layer, x + int(10 * s), y + int(10 * s * (1 - a)), alpha=a)
                if started >= at + 0.35:
                    done += 1
                    frame[y + int(8 * s):y + int(24 * s), x - int(14 * s):x - int(6 * s)] = COLORS[i % 4]
            done_counts.append(done)
            cnt = engine.text_layer([f"完了 {done}/{len(m['steps'])}"], engine.font(engine.FONT_BOLD, int(26 * s)),
                                    COLORS[i % 4])
            engine.paste_rgba(frame, cnt, x, int(height * 0.28) + int((74 + max_steps * 34 + 14) * s))
        # 指標のバー
        if metric and t > intro + max_steps * step_sec + 1.0:
            p = engine.ease_out_cubic((t - (intro + max_steps * step_sec + 1.0)) / 1.2)
            vals = metric["values"]
            vmax = max(vals.values()) or 1
            by = int(height * 0.86)
            lab = engine.text_layer([metric["label"]], engine.font(engine.FONT_MED, int(20 * s)), engine.PALETTE["sub"])
            engine.paste_rgba(frame, lab, int(width * 0.06), by - int(30 * s))
            for i, m in enumerate(methods):
                v = vals.get(m["name"])
                if v is None:
                    continue
                x = gap + i * (col_w + gap)
                w = int((col_w * 0.7) * (v / vmax) * p)
                frame[by:by + int(14 * s), x:x + max(2, w)] = COLORS[i % 4]
                vt = engine.text_layer([f"{v:g}"], engine.font(engine.FONT_BOLD, int(20 * s)), engine.PALETTE["text"])
                engine.paste_rgba(frame, vt, x + max(2, w) + int(8 * s), by - int(6 * s), alpha=p)
        frame = engine.apply_vignette(frame)
        enc.stdin.write(frame.tobytes())
    enc.stdin.close()
    enc.wait()
    return {"out": out_mp4, "duration": round(total, 2), "frames": n_frames, "methods": [m["name"] for m in methods]}


if __name__ == "__main__":
    spec = json.load(open(sys.argv[1], encoding="utf-8"))
    print(json.dumps(render(spec, sys.argv[2]), ensure_ascii=False))
