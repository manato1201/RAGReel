"""同一入力(チュートリアルmd + スクリーンショットmanifest)を3方式で共有するための読み込み。

Qt版(engine/src/ingest/script_composer.cpp)は slide を組み立てるが、比較用実装
(Python版 / HyperFrames版)は同じ入力から同じ「場面リスト」を作る必要がある。
このモジュールがその共通部分で、出力は JSON 化できる素のdict。
"""
from __future__ import annotations

import json
import os
import re
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Optional

_FRONT = re.compile(r"\A---\s*\n(.*?)\n---\s*\n", re.S)
_H2 = re.compile(r"^##\s+(.+?)\s*$", re.M)


@dataclass
class Scene:
    kind: str                      # title | step | bullets
    chapter: str                   # intro | steps | outro  (トランジションは章ごとに固定: V4)
    heading: str = ""
    caption: str = ""              # 画面に出す1〜2文
    narration: str = ""            # 読み上げ文(字幕の元)
    image: str = ""                # 主役の画像パス("" なら画像なし)
    step_index: int = 0            # kind == step のとき 1 始まり
    step_total: int = 0
    bullets: list = field(default_factory=list)
    tool: str = ""
    extra: dict = field(default_factory=dict)


def _strip_md(text: str) -> str:
    text = re.sub(r"```.*?```", "", text, flags=re.S)
    text = re.sub(r"`([^`]*)`", r"\1", text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"\1", text)
    text = re.sub(r"\[\d+\]", "", text)
    return re.sub(r"[ \t]+", " ", text).strip()


def parse_frontmatter(md: str) -> tuple[dict, str]:
    m = _FRONT.match(md)
    if not m:
        return {}, md
    meta = {}
    for line in m.group(1).splitlines():
        if ":" in line:
            k, v = line.split(":", 1)
            meta[k.strip()] = v.strip()
    return meta, md[m.end():]


def split_sections(body: str) -> dict:
    """'## 見出し' ごとに本文を分ける(出現順を保持)。"""
    out: dict = {}
    matches = list(_H2.finditer(body))
    for i, m in enumerate(matches):
        end = matches[i + 1].start() if i + 1 < len(matches) else len(body)
        out[m.group(1)] = body[m.end():end].strip()
    return out


def _bullets(section: str, limit: int = 4) -> list:
    items = []
    for line in section.splitlines():
        line = line.strip()
        if line.startswith(("- ", "* ")):
            items.append(_strip_md(line[2:]))
    return [b for b in items if b][:limit]


def _first_sentences(text: str, n: int = 2) -> str:
    text = _strip_md(text).replace("\n", " ")
    parts = re.split(r"(?<=[。！？])", text)
    return "".join(p for p in parts[:n] if p).strip()


def load_scenes(md_path: str, screenshots_json: Optional[str] = None) -> dict:
    md = Path(md_path).read_text(encoding="utf-8")
    meta, body = parse_frontmatter(md)
    sections = split_sections(body)
    title = meta.get("title") or Path(md_path).stem

    shots = []
    if screenshots_json and Path(screenshots_json).exists():
        shots = json.loads(Path(screenshots_json).read_text(encoding="utf-8"))

    scenes: list[Scene] = []
    summary = _first_sentences(sections.get("概要", ""), 2)
    scenes.append(Scene(kind="title", chapter="intro", heading=title, caption=summary,
                        narration=f"{title}。{summary}".strip("。") + "。"))

    n = len(shots)
    for i, s in enumerate(shots, 1):
        img = s.get("network") if s.get("network") and os.path.exists(s["network"]) else s.get("viewport", "")
        if img and not os.path.exists(img):
            img = ""
        result = _strip_md(s.get("result", ""))
        scenes.append(Scene(kind="step", chapter="steps", heading=f"STEP {i}", caption=result,
                            narration=f"ステップ{i}。{result}", image=img or "",
                            step_index=i, step_total=n, tool=s.get("tool", ""),
                            extra={"viewport": s.get("viewport", ""), "network_kind": s.get("network_kind", "")}))

    pit = _bullets(sections.get("ハマりポイント", ""), 3)
    if pit:
        scenes.append(Scene(kind="bullets", chapter="outro", heading="ハマりポイント", bullets=pit,
                            narration="ハマりポイントです。" + "。".join(pit) + "。"))
    tips = _bullets(sections.get("応用・発展のヒント", ""), 3)
    if tips:
        scenes.append(Scene(kind="bullets", chapter="outro", heading="応用・発展のヒント", bullets=tips,
                            narration="応用のヒントです。" + "。".join(tips) + "。"))
    return {"title": title, "meta": meta, "scenes": [asdict(s) for s in scenes]}


if __name__ == "__main__":
    import sys
    d = load_scenes(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
    print(json.dumps(d, ensure_ascii=False, indent=2)[:3000])
