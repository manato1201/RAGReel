"""文書の検査(ビルド不要。Python 標準ライブラリだけ)。

    python tools/check_docs.py             # 全部
    python tools/check_docs.py --render    # さらに、Mermaid が mmdc で描けるかも確かめる(時間がかかる)

確かめること:
  1. README・docs・計画書の相対リンクと画像参照が、実在するファイルを指す(`#見出し` の飛び先も)
  2. docs/diagrams/*.svg が、README の図の一覧に載っている・どれかの文書から参照されている
  3. SVG が、tools/build_diagrams.py で再現できる(手で直して元のコードとずれていない)・ダークモード対応がある
  4. Mermaid の図が、標準の `%%{init: ...}%%` の行で始まる(GitHub と HTML で色とフォントが揃う)

終了コード: 0 = 問題なし / 1 = 問題あり / 2 = --render で mmdc が無い。
"""
from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

MERMAID_INIT = '%%{init: {"theme":"base","fontFamily":"Noto Sans JP'


def md_files() -> list[Path]:
    files = [ROOT / "README.md", ROOT / "compare" / "README.md"]
    files += sorted(ROOT.glob("IMPROVEMENT*.md"))
    files += sorted((ROOT / "docs").rglob("*.md"))
    return [f for f in files if f.exists()]


def fenced_blocks(text: str):
    """(開始行 0 起点, info, 中身の行) を順に返す。``` と ~~~ の囲みに対応。"""
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        m = re.match(r"^(\s*)(`{3,}|~{3,})\s*([\w-]*)", lines[i])
        if m:
            fence, info, start = m.group(2), m.group(3), i
            body = []
            i += 1
            while i < len(lines) and not re.match(rf"^\s*{re.escape(fence[0])}{{{len(fence)},}}\s*$", lines[i]):
                body.append(lines[i])
                i += 1
            yield start, info, body
        i += 1


def strip_code(text: str) -> str:
    """囲みコードとインラインコードを空にする(中のリンクは検査しない)。行数は保つ。"""
    lines = text.split("\n")
    for start, _info, body in fenced_blocks(text):
        for k in range(start, start + len(body) + 2):
            if k < len(lines):
                lines[k] = ""
    out = "\n".join(lines)
    return re.sub(r"`[^`\n]*`", "", out)


def slug(heading: str) -> str:
    """GitHub の見出しアンカーの近似(小文字化・記号除去・空白をハイフンへ)。"""
    h = re.sub(r"[`*_]", "", heading.strip()).lower()
    h = re.sub(r"[^\w\- 　]", "", h, flags=re.UNICODE)
    return re.sub(r"[ 　]", "-", h)


def anchors_of(path: Path) -> set[str]:
    text = path.read_text(encoding="utf-8")
    skip = set()
    for start, _i, body in fenced_blocks(text):
        skip.update(range(start, start + len(body) + 2))
    out = set()
    for n, line in enumerate(text.split("\n")):
        if n in skip:
            continue
        m = re.match(r"^#{1,6}\s+(.*?)\s*#*\s*$", line)
        if m:
            out.add(slug(m.group(1)))
    return out


LINK = re.compile(r"!?\[[^\]\n]*\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")


def check_links(problems: list[str]) -> None:
    cache: dict[Path, set[str]] = {}
    for path in md_files():
        text = strip_code(path.read_text(encoding="utf-8"))
        for n, line in enumerate(text.split("\n"), 1):
            for m in LINK.finditer(line):
                url = m.group(1)
                if re.match(r"^(https?:|mailto:)", url):
                    continue
                target, _, frag = url.partition("#")
                dest = path if not target else (path.parent / target)
                where = f"{path.relative_to(ROOT)}:{n}"
                if not dest.exists():
                    problems.append(f"{where}: リンク先がない: {url}")
                    continue
                if frag and dest.suffix == ".md" and dest.is_file():
                    anchors = cache.setdefault(dest, anchors_of(dest))
                    if slug(frag) not in anchors and frag.lower() not in anchors:
                        problems.append(f"{where}: 見出しアンカーがない: {url}")


def check_diagram_listing(problems: list[str]) -> None:
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    body = "\n".join(t for t in (p.read_text(encoding="utf-8") for p in md_files() if p.name != "README.md" or p.parent != ROOT))
    for svg in sorted((ROOT / "docs" / "diagrams").glob("*.svg")):
        if f"diagrams/{svg.name}" not in readme:
            problems.append(f"README の図の一覧に載っていない: docs/diagrams/{svg.name}")
        if f"diagrams/{svg.name}" not in body:
            problems.append(f"どの文書(README 以外)からも参照されていない図: docs/diagrams/{svg.name}")


def check_svgs(problems: list[str]) -> None:
    import build_diagrams  # noqa: E402

    with tempfile.TemporaryDirectory() as tmp:
        build_diagrams.OUT = tmp
        import contextlib
        import io

        with contextlib.redirect_stdout(io.StringIO()):
            for fn in build_diagrams.FIGURES:
                fn()
        made = {p.name for p in Path(tmp).glob("*.svg")}
        for svg in sorted((ROOT / "docs" / "diagrams").glob("*.svg")):
            text = svg.read_text(encoding="utf-8").replace("\r\n", "\n")
            if svg.name not in made:
                problems.append(f"build_diagrams.py に対応する関数がない SVG: {svg.name}")
                continue
            fresh = (Path(tmp) / svg.name).read_text(encoding="utf-8")
            if text != fresh:
                problems.append(f"{svg.name}: build_diagrams.py の出力と違う(手で直したか、再生成を忘れている)")
            if "prefers-color-scheme: dark" not in text:
                problems.append(f"{svg.name}: ダークモードの指定がない")
            if not re.search(r'viewBox="0 0 \d+ \d+"', text) or "<title" not in text:
                problems.append(f"{svg.name}: viewBox か title がない")


def mermaid_blocks():
    for path in md_files():
        for start, info, body in fenced_blocks(path.read_text(encoding="utf-8")):
            if info == "mermaid":
                yield path, start + 1, body


def check_mermaid_init(problems: list[str]) -> int:
    total = 0
    for path, line, body in mermaid_blocks():
        total += 1
        if not body or not body[0].startswith(MERMAID_INIT):
            problems.append(f"{path.relative_to(ROOT)}:{line}: Mermaid が標準の init の行で始まらない")
    return total


def render_mermaid(problems: list[str]) -> int:
    mmdc = shutil.which("mmdc")
    if not mmdc:
        print("mmdc が見つかりません(npm install -g @mermaid-js/mermaid-cli)", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory() as tmp:
        for path, line, body in mermaid_blocks():
            src = Path(tmp) / "d.mmd"
            src.write_text("\n".join(body), encoding="utf-8")
            res = subprocess.run([mmdc, "-q", "-i", str(src), "-o", str(Path(tmp) / "d.svg")],
                                 capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=180)
            if res.returncode != 0:
                problems.append(f"{path.relative_to(ROOT)}:{line}: Mermaid を描画できない: {res.stderr.strip()[-200:]}")
    return 0


def main(argv: list[str]) -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    problems: list[str] = []
    check_links(problems)
    check_diagram_listing(problems)
    check_svgs(problems)
    blocks = check_mermaid_init(problems)
    code = 0
    if "--render" in argv:
        code = render_mermaid(problems)
    for p in problems:
        print(p, file=sys.stderr)
    print(f"文書 {len(md_files())} 本・SVG {len(list((ROOT / 'docs' / 'diagrams').glob('*.svg')))} 枚・Mermaid {blocks} 個を検査: 問題 {len(problems)} 件")
    if code == 2:
        return 2
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
