"""V1: HTML + GSAP で場面を書き、HyperFrames で MP4 にする比較実装。

同じ入力(common/tutorial_input.py)・同じ音声と時刻(python版が出した narration/timings.json と
mixed.wav を共有)から HyperFrames プロジェクトを生成する。画面の仕様は python_render と揃える:
  - 見出しは叩きつけて縮む(back.out) / 残像と影 / 画像はゆっくりズーム
  - トランジションは章ごとに固定(intro=fade, steps=slide, outro=hex)
  - STEP n/N と完了数 / 進捗バー / 字幕(読み上げ時刻から)

  python hyperframes/build_project.py --md X.md --shots X_shots.json --narr-dir out/py_work --out out/hf_project
  cd out/hf_project && npx hyperframes@0.8.147 render -o ../hyperframes.mp4
"""
from __future__ import annotations

import argparse
import html
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from common.tutorial_input import load_scenes  # noqa: E402
from python_render.render import plan_boundaries  # noqa: E402  (場面境界の決め方も共有)

HEX_FROM = "polygon(50% 47%, 52% 48.5%, 52% 51.5%, 50% 53%, 48% 51.5%, 48% 48.5%)"
HEX_TO = "polygon(50% -60%, 160% -5%, 160% 105%, 50% 160%, -60% 105%, -60% -5%)"
TRANSITION = {"intro": "fade", "steps": "slide", "outro": "hex"}
HALF = 0.25
FONT_DIR = r"C:\Windows\Fonts"

CSS = """
@font-face { font-family: "NotoSansJPLocal"; src: url("assets/NotoSansJP-Bold.otf"); font-weight: 700; }
@font-face { font-family: "NotoSansJPLocal"; src: url("assets/NotoSansJP-Medium.otf"); font-weight: 500; }
* { margin: 0; padding: 0; box-sizing: border-box; }
html, body { margin: 0; width: 1280px; height: 720px; overflow: hidden; background: #121421; }
#root { position: relative; width: 100%; height: 100%; overflow: hidden;
        font-family: "NotoSansJPLocal", sans-serif; color: #f5f4fa; }
.scene { position: absolute; inset: 0; overflow: hidden; }
.bg { position: absolute; inset: 0; background: linear-gradient(180deg, #121421 0%, #241c38 100%); }
.vig { position: absolute; inset: 0; background: radial-gradient(ellipse at center, rgba(0,0,0,0) 55%, rgba(0,0,0,.38) 100%); }
.head { position: absolute; left: 77px; top: 58px; font-size: 54px; font-weight: 700; line-height: 1.25;
        white-space: nowrap; text-shadow: 5px 7px 12px rgba(0,0,0,.5); }
.ghost { position: absolute; left: 77px; top: 58px; font-size: 54px; font-weight: 700; line-height: 1.25;
         white-space: nowrap; opacity: 0; }
.title .head, .title .ghost { top: 216px; left: 102px; }
.cap { position: absolute; left: 77px; top: 178px; width: 460px; font-size: 28px; font-weight: 500;
       line-height: 1.55; color: #b0acc4; line-break: strict; overflow-wrap: anywhere; }
.title .cap { top: 336px; left: 102px; width: 1050px; }
.tag { position: absolute; left: 77px; top: 18px; font-size: 24px; font-weight: 700; color: #ff8a4c; }
.hero { position: absolute; box-shadow: 0 24px 50px rgba(0,0,0,.6); }
.bul { position: absolute; left: 102px; width: 1000px; font-size: 30px; font-weight: 500; line-height: 1.5; }
.bar { position: absolute; left: 77px; top: 700px; height: 6px; background: #60d6a8; }
.sub { position: absolute; left: 0; right: 0; top: 572px; display: flex; justify-content: center; }
.sub span { background: rgba(0,0,0,.65); padding: 9px 18px; font-size: 30px; font-weight: 500; line-height: 1.4;
            max-width: 1075px; text-align: center; line-break: strict; text-wrap: balance; }
"""


def build(md, shots, narr_dir, out_dir, bgm_bpm=100.0):
    data = load_scenes(md, shots)
    scenes = data["scenes"]
    timings = json.load(open(os.path.join(narr_dir, "narr", "timings.json"), encoding="utf-8"))
    segs = timings["segments"]
    ranges = timings["scene_ranges"]
    mixed = os.path.join(narr_dir, "mixed.wav")
    audio_src = mixed if os.path.exists(mixed) else timings["wav"]

    # 場面境界は python 版と同じ関数(拍に寄せる場合は同じ BGM の拍)
    beat_list = []
    if os.path.exists(os.path.join(narr_dir, "bgm.wav")):
        from common import beats as beatlib
        beat_list = beatlib.detect(os.path.join(narr_dir, "bgm.wav"))["beats"]
    starts = plan_boundaries(ranges, len(scenes), beat_list)
    total = round(timings["duration"] + 1.5, 3)
    ends = starts[1:] + [total]

    os.makedirs(os.path.join(out_dir, "assets"), exist_ok=True)
    shutil.copy(audio_src, os.path.join(out_dir, "assets", "narration.wav"))
    for f in ("NotoSansJP-Bold.otf", "NotoSansJP-Medium.otf"):
        shutil.copy(os.path.join(FONT_DIR, f), os.path.join(out_dir, "assets", f))

    n_steps = sum(1 for s in scenes if s["kind"] == "step")
    parts, js = [], []
    completed = 0
    for i, sc in enumerate(scenes):
        s0 = starts[i]
        win0 = s0 - HALF if i > 0 else 0.0
        win1 = ends[i] + HALF if i + 1 < len(scenes) else total
        z = i + 1
        cls = "scene title" if sc["kind"] == "title" else "scene"
        inner = ['<div class="bg"></div>']
        if sc["kind"] == "step":
            inner.append('<div class="tag">STEP %d/%d　完了 %d</div>' % (sc["step_index"], n_steps, completed))
        inner.append('<div class="ghost" id="g%d">%s</div>' % (i, html.escape(sc["heading"])))
        inner.append('<div class="head" id="h%d">%s</div>' % (i, html.escape(sc["heading"])))
        if sc.get("caption"):
            inner.append('<p class="cap" id="c%d">%s</p>' % (i, html.escape(sc["caption"])))
        for b, text in enumerate(sc.get("bullets", [])):
            inner.append('<p class="bul" id="b%d_%d" style="top:%dpx">%s</p>' % (i, b, 216 + b * 130, html.escape(text)))
        if sc.get("image") and os.path.exists(sc["image"]):
            name = "img_%d.png" % i
            shutil.copy(sc["image"], os.path.join(out_dir, "assets", name))
            from PIL import Image
            iw, ih = Image.open(sc["image"]).size
            r = min(665.6 / iw, 518.4 / ih)
            hw, hh = int(iw * r), int(ih * r)
            hx = int(563.2 + (665.6 - hw) / 2 + 25.6)
            hy = int(86.4 + (518.4 - hh) / 2)
            inner.append('<img class="hero" id="i%d" src="assets/%s" style="left:%dpx;top:%dpx;width:%dpx;height:%dpx">'
                         % (i, name, hx, hy, hw, hh))
        if sc["kind"] == "step":
            done = completed
            inner.append('<div class="bar" id="p%d" style="width:%dpx"></div>' % (i, int(1126 * done / max(1, n_steps))))
        inner.append('<div class="vig"></div>')
        parts.append('<div class="clip %s" id="s%d" data-start="%.3f" data-duration="%.3f" style="z-index:%d">%s</div>'
                     % (cls, i, win0, win1 - win0, z, "".join(inner)))

        # --- アニメーション(絶対時刻 s0 を基準) ---
        L = []
        L.append('tl.fromTo("#h%d",{scale:1.55,opacity:.2,transformOrigin:"left center"},'
                 '{scale:1,opacity:1,duration:.45,ease:"back.out(1.9)"},%.3f);' % (i, s0))
        L.append('tl.fromTo("#g%d",{scale:1.7,opacity:.25,transformOrigin:"left center"},'
                 '{scale:1.2,opacity:0,duration:.6,ease:"power2.out"},%.3f);' % (i, s0))
        if sc.get("caption"):
            L.append('tl.fromTo("#c%d",{opacity:0,y:18},{opacity:1,y:0,duration:.5,ease:"power3.out"},%.3f);' % (i, s0 + .25))
        for b in range(len(sc.get("bullets", []))):
            L.append('tl.fromTo("#b%d_%d",{opacity:0,x:40},{opacity:1,x:0,duration:.45,ease:"power3.out"},%.3f);'
                     % (i, b, s0 + .35 + .25 * b))
        if sc.get("image") and os.path.exists(sc["image"]):
            L.append('tl.fromTo("#i%d",{opacity:0,y:14,scale:1},{opacity:1,y:0,duration:.45,ease:"power3.out"},%.3f);' % (i, s0 + .15))
            L.append('tl.to("#i%d",{scale:1.04,duration:6,ease:"none"},%.3f);' % (i, s0 + .6))
        # 入場トランジション(章ごとに固定)
        if i > 0:
            kind = TRANSITION.get(sc["chapter"], "fade")
            t0 = s0 - HALF
            if kind == "fade":
                L.append('tl.fromTo("#s%d",{opacity:0},{opacity:1,duration:%.2f,ease:"none"},%.3f);' % (i, 2 * HALF, t0))
            elif kind == "slide":
                L.append('tl.fromTo("#s%d",{x:1280},{x:0,duration:%.2f,ease:"power3.out"},%.3f);' % (i, 2 * HALF, t0))
            else:
                L.append('tl.fromTo("#s%d",{clipPath:"%s"},{clipPath:"%s",duration:%.2f,ease:"power2.inOut"},%.3f);'
                         % (i, HEX_FROM, HEX_TO, 2 * HALF, t0))
        js.extend(L)
        if sc["kind"] == "step":
            completed += 1

    subs = []
    for k, sg in enumerate(segs):
        a = max(0.0, sg["start"] - 0.05)
        b = min(total, sg["end"] + 0.15)
        subs.append('<div class="clip sub" id="u%d" data-start="%.3f" data-duration="%.3f" style="z-index:900">'
                    '<span>%s</span></div>' % (k, a, b - a, html.escape(sg["text"])))

    doc = """<!doctype html>
<html lang="ja">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=1280, height=720">
<script src="https://cdn.jsdelivr.net/npm/gsap@3.14.2/dist/gsap.min.js"></script>
<style>%s</style>
</head>
<body>
<div id="root" data-composition-id="main" data-start="0" data-duration="%.3f" data-width="1280" data-height="720">
%s
%s
<audio id="narration" data-start="0" data-duration="%.3f" src="assets/narration.wav" data-volume="1"></audio>
</div>
<script>
const tl = gsap.timeline({ paused: true });
%s
window.__timelines["main"] = tl;
tl.seek(0);
</script>
</body>
</html>
""" % (CSS, total, "\n".join(parts), "\n".join(subs), timings["duration"], "\n".join(js))
    with open(os.path.join(out_dir, "index.html"), "w", encoding="utf-8") as f:
        f.write(doc)
    with open(os.path.join(out_dir, "hyperframes.json"), "w", encoding="utf-8") as f:
        json.dump({"$schema": "https://hyperframes.heygen.com/schema/hyperframes.json",
                   "paths": {"blocks": "compositions", "components": "compositions/components", "assets": "assets"}},
                  f, indent=2)
    with open(os.path.join(out_dir, "meta.json"), "w", encoding="utf-8") as f:
        json.dump({"id": "ragreel-compare", "name": "ragreel-compare"}, f)
    return {"duration": total, "scenes": len(scenes), "lines_html": doc.count("\n"), "out": out_dir}


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--md", required=True)
    ap.add_argument("--shots")
    ap.add_argument("--narr-dir", required=True, help="python_render が作った py_work ディレクトリ")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    print(json.dumps(build(a.md, a.shots, a.narr_dir, a.out), ensure_ascii=False))
