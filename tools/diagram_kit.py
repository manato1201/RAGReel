"""Small SVG kit for docs/diagrams (architecture + flowchart figures).

The style follows the reference figures the project adopted (DNS flow, the
Cloudflare stack map, the AWS layout): grouped regions with a label tab,
icon tiles with a bold title and a grey monospace caption, numbered step
badges, solid arrows for the main flow and dashed grey arrows for secondary
flows. Colours reuse the dashboard's role colours (docs/ui-rules.md):
orange = main action, mint = done, pink = error, blue = information,
yellow = caution, purple = RAG.

Nothing here is clever on purpose: each helper emits plain SVG so the
generated files stay readable and can also be edited by hand.
"""
from xml.sax.saxutils import escape

SANS = "'Noto Sans JP','Yu Gothic UI','Meiryo',system-ui,sans-serif"
MONO = "'JetBrains Mono',Consolas,'Courier New',monospace"

INK = "#1f2937"
MUTED = "#6b7280"
LINE = "#d4d4d8"
PAGE = "#fbfaf8"
GRID = "#efece6"

# role colours: (stroke/ink, soft tint)
ORANGE = ("#d9591f", "#fdf0e8")
MINT = ("#1a7a53", "#e6f6ee")
PINK = ("#b52b57", "#fde9f0")
BLUE = ("#1d63d8", "#e8f0fd")
YELLOW = ("#8a6400", "#fdf4d9")
PURPLE = ("#6d4fc2", "#f0ebfb")
GRAY = ("#4b5563", "#f1f1f3")

ICONS = {
    "user": '<circle cx="12" cy="8" r="4"/><path d="M4 21c0-4.4 3.6-8 8-8s8 3.6 8 8"/>',
    "monitor": '<rect x="3" y="4" width="18" height="12" rx="2"/><path d="M8 20h8M12 16v4"/>',
    "server": '<rect x="3" y="4" width="18" height="6" rx="1.5"/><rect x="3" y="14" width="18" height="6" rx="1.5"/><path d="M7 7h.01M7 17h.01"/>',
    "cloud": '<path d="M6.5 18h11a4 4 0 0 0 .5-7.97A6 6 0 0 0 6.6 9.6 4.2 4.2 0 0 0 6.5 18z"/>',
    "bucket": '<ellipse cx="12" cy="6" rx="8" ry="3"/><path d="M4 6v12c0 1.7 3.6 3 8 3s8-1.3 8-3V6M4 12c0 1.7 3.6 3 8 3s8-1.3 8-3"/>',
    "file": '<path d="M6 3h8l4 4v14H6z"/><path d="M14 3v4h4M9 13h6M9 17h6"/>',
    "film": '<rect x="3" y="5" width="18" height="14" rx="2"/><path d="M10 9.5v5l4.5-2.5z"/>',
    "mic": '<rect x="9" y="3" width="6" height="11" rx="3"/><path d="M5 11a7 7 0 0 0 14 0M12 18v3"/>',
    "gear": '<circle cx="12" cy="12" r="3"/><path d="M12 2v3M12 19v3M2 12h3M19 12h3M5 5l2 2M17 17l2 2M19 5l-2 2M7 17l-2 2"/>',
    "globe": '<circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3c3 3 3 15 0 18M12 3c-3 3-3 15 0 18"/>',
    "key": '<circle cx="8" cy="15" r="4"/><path d="M11 12l9-9M16 7l3 3"/>',
    "layers": '<path d="M12 3 3 8l9 5 9-5-9-5z"/><path d="m3 13 9 5 9-5"/>',
    "code": '<path d="m8 8-5 4 5 4M16 8l5 4-5 4M14 5l-4 14"/>',
    "subtitle": '<rect x="3" y="5" width="18" height="14" rx="2"/><path d="M7 11h4M13 11h4M7 15h10"/>',
    "list": '<path d="M4 6h16M4 12h10M4 18h16"/>',
    "play": '<circle cx="12" cy="12" r="9"/><path d="M10 8.5v7l6-3.5z"/>',
    "bolt": '<path d="M13 2 4 14h7l-1 8 9-12h-7z"/>',
    "terminal": '<rect x="3" y="4" width="18" height="16" rx="2"/><path d="m7 10 3 2-3 2M12 15h5"/>',
    "shield": '<path d="M12 3 4 6v6c0 5 3.5 8 8 9 4.5-1 8-4 8-9V6z"/><path d="m9 12 2 2 4-4"/>',
    "upload": '<path d="M12 16V4M7 9l5-5 5 5M4 20h16"/>',
    "search": '<circle cx="11" cy="11" r="7"/><path d="m20 20-4-4"/>',
    "chart": '<path d="M5 20V10M12 20V4M19 20v-7"/>',
    "image": '<rect x="3" y="3" width="18" height="18" rx="2"/><path d="M3 15l5-5 4 4 3-3 6 6"/>',
    "clock": '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>',
    "folder": '<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>',
    "cube": '<path d="M12 3 4 7v10l8 4 8-4V7z"/><path d="m4 7 8 4 8-4M12 11v10"/>',
    "wave": '<path d="M3 12h2l2-6 3 12 3-9 2 5 2-2h4"/>',
    "check": '<circle cx="12" cy="12" r="9"/><path d="m8 12 3 3 5-6"/>',
    "branch": '<circle cx="6" cy="5" r="2"/><circle cx="6" cy="19" r="2"/><circle cx="18" cy="9" r="2"/><path d="M6 7v10M18 11c0 4-6 3-12 6"/>',
}


def tw(text, size):
    """Rough text width: full-width glyphs ~1em, ASCII ~0.58em."""
    return sum(size if ord(c) > 255 else size * 0.58 for c in text)


class Svg:
    def __init__(self, width, height, title, desc):
        self.w, self.h = width, height
        self.title, self.desc = title, desc
        self.parts = []

    # -- low level -----------------------------------------------------
    def add(self, s):
        self.parts.append(s)

    def text(self, x, y, s, size=14, weight=400, fill=INK, anchor="middle", mono=False):
        family = MONO if mono else SANS
        self.add(
            f'<text x="{x:g}" y="{y:g}" font-family="{family}" font-size="{size}" '
            f'font-weight="{weight}" fill="{fill}" text-anchor="{anchor}">{escape(s)}</text>'
        )

    def lines(self, x, y, rows, size=13, gap=18, **kw):
        for i, row in enumerate(rows):
            self.text(x, y + i * gap, row, size=size, **kw)

    # -- regions -------------------------------------------------------
    def group(self, x, y, w, h, label, color, dashed=False, sub=None):
        ink, tint = color
        dash = ' stroke-dasharray="6 5"' if dashed else ""
        self.add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="16" fill="{tint}" '
                 f'stroke="{ink}" stroke-width="1.5"{dash}/>')
        text = label + (f"  {sub}" if sub else "")
        lw = tw(label, 13) + (tw("  " + sub, 11) if sub else 0) + 20
        self.add(f'<rect x="{x + 18}" y="{y - 11}" width="{lw:g}" height="22" fill="{PAGE}"/>')
        self.add(f'<text x="{x + 28}" y="{y + 5}" font-family="{SANS}" font-size="13" '
                 f'font-weight="700" fill="{ink}" letter-spacing="0.5">{escape(label)}'
                 + (f'<tspan font-family="{MONO}" font-size="11" font-weight="400" fill="{MUTED}">'
                    f'{escape("  " + sub)}</tspan>' if sub else "")
                 + "</text>")

    # -- nodes ---------------------------------------------------------
    def icon(self, cx, cy, name, color=INK, size=32):
        s = size / 24
        self.add(f'<g transform="translate({cx - size / 2:g} {cy - size / 2:g}) scale({s:g})" fill="none" '
                 f'stroke="{color}" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">'
                 f'{ICONS[name]}</g>')

    def tile(self, cx, cy, icon, title, sub=None, color=INK, size=64, fill="#ffffff"):
        x, y = cx - size / 2, cy - size / 2
        self.add(f'<rect x="{x:g}" y="{y:g}" width="{size}" height="{size}" rx="13" fill="{fill}" '
                 f'stroke="{LINE}" stroke-width="1.5"/>')
        self.icon(cx, cy, icon, color)
        ty = cy + size / 2 + 22
        self.text(cx, ty, title, size=15, weight=700)
        if sub:
            self.text(cx, ty + 18, sub, size=11, fill=MUTED, mono=True)

    def box(self, cx, cy, w, h, rows, color=GRAY, size=13, weight=600, dashed=False):
        ink, tint = color
        dash = ' stroke-dasharray="5 4"' if dashed else ""
        self.add(f'<rect x="{cx - w / 2:g}" y="{cy - h / 2:g}" width="{w}" height="{h}" rx="10" '
                 f'fill="{tint}" stroke="{ink}" stroke-width="1.4"{dash}/>')
        n = len(rows)
        gap = size + 5
        y0 = cy - (n - 1) * gap / 2 + size * 0.35
        for i, row in enumerate(rows):
            self.text(cx, y0 + i * gap, row, size=size, weight=weight if i == 0 else 400,
                      fill=INK if i == 0 else MUTED)

    def pill(self, cx, cy, w, h, text, color=MINT, size=13):
        ink, tint = color
        self.add(f'<rect x="{cx - w / 2:g}" y="{cy - h / 2:g}" width="{w}" height="{h}" rx="{h / 2:g}" '
                 f'fill="{tint}" stroke="{ink}" stroke-width="1.5"/>')
        self.text(cx, cy + size * 0.35, text, size=size, weight=700, fill=ink)

    def diamond(self, cx, cy, w, h, rows, color=YELLOW, size=13):
        ink, tint = color
        pts = f"{cx},{cy - h / 2:g} {cx + w / 2:g},{cy} {cx},{cy + h / 2:g} {cx - w / 2:g},{cy}"
        self.add(f'<polygon points="{pts}" fill="{tint}" stroke="{ink}" stroke-width="1.5"/>')
        n = len(rows)
        gap = size + 4
        y0 = cy - (n - 1) * gap / 2 + size * 0.35
        for i, row in enumerate(rows):
            self.text(cx, y0 + i * gap, row, size=size, weight=700 if i == 0 else 400,
                      fill=INK)

    def badge(self, x, y, n, color="#1d63d8"):
        self.add(f'<circle cx="{x:g}" cy="{y:g}" r="12" fill="{color}"/>')
        self.text(x, y + 4.5, str(n), size=13, weight=700, fill="#ffffff")

    def label(self, x, y, s, color=MUTED, size=12, mono=False):
        w = tw(s, size) + 10
        self.add(f'<rect x="{x - w / 2:g}" y="{y - size:g}" width="{w:g}" height="{size + 7}" rx="4" fill="{PAGE}"/>')
        self.text(x, y, s, size=size, fill=color, mono=mono, weight=600)

    # -- connectors ----------------------------------------------------
    def arrow(self, pts, kind="main", both=False):
        """pts: list of (x, y). kind: main (blue) | accent (orange) | dash (grey) | ok (mint)."""
        stroke, dash, marker = {
            "main": ("#1d63d8", "", "ab"),
            "accent": ("#d9591f", "", "ao"),
            "ok": ("#1a7a53", "", "am"),
            "dash": ("#6b7280", ' stroke-dasharray="6 5"', "ag"),
        }[kind]
        d = "M" + " L".join(f"{x:g} {y:g}" for x, y in pts)
        start = f' marker-start="url(#{marker}s)"' if both else ""
        self.add(f'<path d="{d}" fill="none" stroke="{stroke}" stroke-width="2"{dash} '
                 f'stroke-linejoin="round" marker-end="url(#{marker})"{start}/>')

    # -- output --------------------------------------------------------
    def render(self):
        def marker(mid, color, start=False):
            if start:
                return (f'<marker id="{mid}s" viewBox="0 0 10 10" refX="2" refY="5" markerWidth="8" '
                        f'markerHeight="8" orient="auto-start-reverse"><path d="M10 0 0 5l10 5z" fill="{color}"/></marker>')
            return (f'<marker id="{mid}" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="8" '
                    f'markerHeight="8" orient="auto"><path d="M0 0l10 5-10 5z" fill="{color}"/></marker>')

        defs = "".join(
            marker(m, c, s) for m, c in
            (("ab", "#1d63d8"), ("ao", "#d9591f"), ("am", "#1a7a53"), ("ag", "#6b7280")) for s in (False, True)
        )
        grid = (f'<pattern id="grid" width="24" height="24" patternUnits="userSpaceOnUse">'
                f'<path d="M24 0H0V24" fill="none" stroke="{GRID}" stroke-width="1"/></pattern>')
        head = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {self.w} {self.h}" '
                f'width="{self.w}" height="{self.h}" role="img" aria-labelledby="t d">'
                f'<title id="t">{escape(self.title)}</title><desc id="d">{escape(self.desc)}</desc>'
                f'<defs>{defs}{grid}</defs>'
                f'<rect width="{self.w}" height="{self.h}" fill="{PAGE}"/>'
                f'<rect width="{self.w}" height="{self.h}" fill="url(#grid)"/>')
        return head + "\n".join(self.parts) + "</svg>\n"

    def save(self, path):
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(self.render())


def legend(svg, x, y, items, col_w=560, size=13, row_h=26):
    """Numbered step list under a figure. items: [(n, text), ...] in two columns."""
    half = (len(items) + 1) // 2
    for i, (n, text) in enumerate(items):
        col, row = divmod(i, half)
        cx = x + col * col_w
        cy = y + row * row_h
        svg.badge(cx + 12, cy, n)
        svg.text(cx + 32, cy + 4.5, text, size=size, anchor="start")
