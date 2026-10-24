"""V2: Python(NumPy + Pillow + ffmpeg)だけで合成するレンダラ。GPU・Qt・ブラウザ不要。

レイヤーは 背景 → 文字 → 残像/影 → 主役(画像) → 仕上げ の順(IMPROVEMENT_PLAN_2026-10 Phase 2)。
文字は「大きい状態から叩きつけて縮む」オーバーシュート付きイージング。
トランジションは、既定は3種(fade / slide / hex)で章ごとに固定(毎回変えない: V4)。
候補は名前で増やせるが、1本の動画で使う種類は3以内(validate_transition_plan)。
画面には STEP n/N と「完了数」を出す(V5)。字幕は読み上げの時刻情報から(V7)。
"""
from __future__ import annotations

import math
import os
import re
import subprocess
from functools import lru_cache
from typing import Optional

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONT_BOLD = os.environ.get("RAGREEL_FONT_BOLD", r"C:\Windows\Fonts\NotoSansJP-Bold.otf")
FONT_MED = os.environ.get("RAGREEL_FONT_MED", r"C:\Windows\Fonts\NotoSansJP-Medium.otf")

# 章 → トランジション(固定)。毎回変えない。
CHAPTER_TRANSITION = {"intro": "fade", "steps": "slide", "outro": "hex"}
TRANSITION_SEC = 0.5

# 候補(いずれも「時刻 p だけで絵が決まる純関数」なので、1コマずつ描く方式と相性が良い)。
# mask=斜めのマスクワイプ / shape=円の広がり / liquid=波打つ境界 / blinds=ブラインド / pixelate=ブロック単位。
SUPPORTED_TRANSITIONS = ("fade", "slide", "hex", "mask", "shape", "liquid", "blinds", "pixelate")
TRANSITION_CHAPTERS = ("intro", "steps", "outro")
MAX_TRANSITION_KINDS = 3  # 1本の動画で使う種類の上限(V4の方針)


def validate_transition_plan(plan: dict) -> dict:
    """章→種類の対応を検証して返す。未知の章・種類、または種類が3を超える場合は ValueError。"""
    for chapter, kind in plan.items():
        if chapter not in TRANSITION_CHAPTERS:
            raise ValueError("unknown chapter %r (use %s)" % (chapter, ", ".join(TRANSITION_CHAPTERS)))
        if kind not in SUPPORTED_TRANSITIONS:
            raise ValueError("unknown transition %r (use %s)" % (kind, ", ".join(SUPPORTED_TRANSITIONS)))
    kinds = set(plan.values())
    if len(kinds) > MAX_TRANSITION_KINDS:
        raise ValueError("%d transition kinds in one video (max %d): %s"
                         % (len(kinds), MAX_TRANSITION_KINDS, ", ".join(sorted(kinds))))
    return dict(plan)


def parse_transition_plan(spec: str, base: dict = None) -> dict:
    """'steps=mask,outro=shape' を、既定(CHAPTER_TRANSITION)に上書きして検証する。"""
    plan = dict(base if base is not None else CHAPTER_TRANSITION)
    for part in (p.strip() for p in (spec or "").split(",")):
        if not part:
            continue
        chapter, sep, kind = part.partition("=")
        if not sep:
            raise ValueError("expected chapter=kind, got %r" % part)
        plan[chapter.strip()] = kind.strip()
    return validate_transition_plan(plan)

PALETTE = {
    "bg_top": (18, 20, 33), "bg_bot": (36, 28, 56), "text": (245, 244, 250), "sub": (176, 172, 196),
    "accent": (255, 138, 76), "mint": (96, 214, 168), "panel": (30, 30, 48), "shadow": (0, 0, 0),
}


@lru_cache(maxsize=64)
def font(path: str, size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(path, size)


def ease_out_back(t: float, s: float = 1.9) -> float:
    t = min(1.0, max(0.0, t)) - 1.0
    return 1.0 + (s + 1.0) * t ** 3 + s * t ** 2


def ease_out_cubic(t: float) -> float:
    t = min(1.0, max(0.0, t))
    return 1.0 - (1.0 - t) ** 3


_WORD = re.compile(r"[A-Za-z0-9_:.@/+\-]+|.", re.S)


def wrap(text: str, fnt: ImageFont.FreeTypeFont, max_w: int) -> list:
    """幅で折り返す。英数字の単語(attribwrangle 等)は途中で割らない。"""
    lines, cur = [], ""
    for unit in _WORD.findall(text):
        if fnt.getlength(cur + unit) > max_w and cur:
            lines.append(cur)
            cur = unit.lstrip()
        else:
            cur += unit
    if cur:
        lines.append(cur)
    return lines


def text_layer(lines: list, fnt, color, line_gap: int = 10) -> Image.Image:
    asc, desc = fnt.getmetrics()
    lh = asc + desc + line_gap
    w = max([int(fnt.getlength(l)) for l in lines] + [0]) + 8
    img = Image.new("RGBA", (w, lh * max(1, len(lines)) + 8), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for i, l in enumerate(lines):
        d.text((4, 4 + i * lh), l, font=fnt, fill=tuple(color) + (255,))
    return img


@lru_cache(maxsize=4)
def background(w: int, h: int) -> np.ndarray:
    top, bot = np.array(PALETTE["bg_top"], np.float32), np.array(PALETTE["bg_bot"], np.float32)
    t = np.linspace(0, 1, h, dtype=np.float32)[:, None, None]
    img = (top * (1 - t) + bot * t) * np.ones((1, w, 1), np.float32)
    return img.astype(np.uint8)


@lru_cache(maxsize=4)
def vignette(w: int, h: int) -> np.ndarray:
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    d = np.sqrt(((x - w / 2) / (w / 2)) ** 2 + ((y - h / 2) / (h / 2)) ** 2)
    return (1.0 - 0.38 * np.clip(d - 0.55, 0, 1) ** 1.5)[:, :, None]


def paste_rgba(dst: np.ndarray, layer: Image.Image, x: int, y: int, alpha: float = 1.0):
    """RGBA の Pillow 画像を ndarray(RGB uint8) にアルファ合成する。範囲外は切る。"""
    H, W = dst.shape[:2]
    lw, lh = layer.size
    x0, y0, x1, y1 = max(0, x), max(0, y), min(W, x + lw), min(H, y + lh)
    if x0 >= x1 or y0 >= y1 or alpha <= 0:
        return
    sub = np.asarray(layer.crop((x0 - x, y0 - y, x1 - x, y1 - y)), dtype=np.float32)
    a = (sub[:, :, 3:4] / 255.0) * min(1.0, alpha)
    region = dst[y0:y1, x0:x1].astype(np.float32)
    dst[y0:y1, x0:x1] = (region * (1 - a) + sub[:, :, :3] * a).astype(np.uint8)


class Scene:
    """1場面の静的素材(文字レイヤー・画像)をあらかじめ作っておき、フレームごとに動かすだけにする。"""

    def __init__(self, spec: dict, w: int, h: int):
        self.spec, self.w, self.h = spec, w, h
        self.kind = spec["kind"]
        s = w / 1280.0
        self.s = s
        self.heading = text_layer([spec.get("heading", "")], font(FONT_BOLD, int(54 * s)), PALETTE["text"])
        self.caption_lines = wrap(spec.get("caption", ""), font(FONT_MED, int(28 * s)),
                                  int(w * (0.82 if self.kind == "title" else 0.36)))[:5]
        self.caption = text_layer(self.caption_lines, font(FONT_MED, int(28 * s)), PALETTE["sub"]) if self.caption_lines else None
        self.bullets = [text_layer(wrap(b, font(FONT_MED, int(30 * s)), int(w * 0.8))[:2],
                                   font(FONT_MED, int(30 * s)), PALETTE["text"]) for b in spec.get("bullets", [])]
        self.hero: Optional[Image.Image] = None
        img = spec.get("image")
        if img and os.path.exists(img):
            im = Image.open(img).convert("RGB")
            box_w, box_h = int(w * 0.52), int(h * 0.72)
            r = min(box_w / im.width, box_h / im.height)
            self.hero = im.resize((max(1, int(im.width * r)), max(1, int(im.height * r))), Image.LANCZOS)

    def render(self, t: float, completed: int, total_steps: int) -> np.ndarray:
        w, h, s = self.w, self.h, self.s
        frame = background(w, h).copy()

        # 文字(2): 見出しは叩きつけて縮む。残像(3): 同じ形を少し大きく薄く、影を下に。
        # 大きい状態(1.55倍)から叩きつけて縮み、少し行き過ぎて(オーバーシュート)1.0に収まる
        scale = 1.0 + 0.55 * (1.0 - ease_out_back(t / 0.45)) if t < 0.45 else 1.0
        hx, hy = int(w * 0.06), int(h * 0.08)
        if self.kind == "title":
            hx, hy = int(w * 0.08), int(h * 0.30)
        if t < 0.6:
            gs = scale * 1.12
            gl = self.heading.resize((max(1, int(self.heading.width * gs)), max(1, int(self.heading.height * gs))))
            paste_rgba(frame, gl, hx - int((gl.width - self.heading.width) / 2),
                       hy - int((gl.height - self.heading.height) / 2), alpha=0.25 * (1 - t / 0.6))
        hl = self.heading.resize((max(1, int(self.heading.width * scale)), max(1, int(self.heading.height * scale))))
        shadow = Image.new("RGBA", hl.size, (0, 0, 0, 0))
        shadow.putalpha(hl.split()[3].point(lambda v: int(v * 0.5)))
        shadow = shadow.filter(ImageFilter.GaussianBlur(max(1, int(6 * s))))
        paste_rgba(frame, shadow, hx + int(5 * s), hy + int(7 * s))
        paste_rgba(frame, hl, hx, hy, alpha=min(1.0, t / 0.12 + 0.2))

        if self.caption is not None:
            ct = ease_out_cubic((t - 0.25) / 0.5)
            cy = hy + int(120 * s)
            paste_rgba(frame, self.caption, hx, cy + int(18 * s * (1 - ct)), alpha=ct)

        for i, b in enumerate(self.bullets):
            bt = ease_out_cubic((t - 0.35 - 0.25 * i) / 0.45)
            by = int(h * 0.30) + i * int(130 * s)
            paste_rgba(frame, b, hx + int(40 * s * (1 - bt)), by, alpha=bt)

        # 主役(4): 画像。ゆっくりズーム(Ken Burns)+ 影
        if self.hero is not None:
            z = 1.0 + 0.04 * min(1.0, t / 6.0)
            hw, hh = int(self.hero.width * z), int(self.hero.height * z)
            hero = self.hero.resize((hw, hh)).convert("RGBA")
            x = int(w * 0.44) + (int(w * 0.52) - hw) // 2 + int(w * 0.02)
            y = int(h * 0.12) + (int(h * 0.72) - hh) // 2
            sh = Image.new("RGBA", (hw + 40, hh + 40), (0, 0, 0, 0))
            ImageDraw.Draw(sh).rectangle((20, 24, hw + 20, hh + 24), fill=(0, 0, 0, 150))
            sh = sh.filter(ImageFilter.GaussianBlur(14))
            ha = ease_out_cubic((t - 0.15) / 0.45)
            paste_rgba(frame, sh, x - 20, y - 20, alpha=ha)
            paste_rgba(frame, hero, x, y + int(14 * (1 - ha)), alpha=ha)

        # 仕上げ(5): ステップ表示・完了数・進捗バー
        if self.kind == "step":
            lab = "STEP %d/%d   完了 %d" % (self.spec["step_index"], total_steps, completed)
            lt = text_layer([lab], font(FONT_BOLD, int(24 * s)), PALETTE["accent"])
            paste_rgba(frame, lt, int(w * 0.06), int(h * 0.025))
            bar_w = int((w * 0.88) * (completed / max(1, total_steps)))
            y0 = int(h * 0.972)  # 2行字幕(中心 0.865h)と重ならない位置
            frame[y0:y0 + max(4, int(6 * s)), int(w * 0.06):int(w * 0.06) + bar_w] = PALETTE["mint"]
        return frame


def apply_vignette(frame: np.ndarray) -> np.ndarray:
    h, w = frame.shape[:2]
    return (frame.astype(np.float32) * vignette(w, h)).astype(np.uint8)


# ---------------- transitions ----------------
@lru_cache(maxsize=2)
def hex_thresholds(w: int, h: int, size: int = 72) -> np.ndarray:
    """六角形セルごとに「開く順番」(0..1)を持つ画素マップ。左上→右下 + セルごとのゆらぎ。"""
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    q = (math.sqrt(3) / 3 * x - 1.0 / 3 * y) / size
    r = (2.0 / 3 * y) / size
    cx, cz = q, r
    cy = -cx - cz
    rx, ry, rz = np.round(cx), np.round(cy), np.round(cz)
    dx, dy, dz = np.abs(rx - cx), np.abs(ry - cy), np.abs(rz - cz)
    fix_x = (dx > dy) & (dx > dz)
    fix_y = (~fix_x) & (dy > dz)
    rx = np.where(fix_x, -ry - rz, rx)
    rz = np.where(~fix_x & ~fix_y, -rx - ry, rz)
    key = ((rx * 73856093 + rz * 19349663).astype(np.int64)) % 1000
    jitter = key.astype(np.float32) / 1000.0
    diag = rx * size * math.sqrt(3) + rz * size * 1.5
    diag = (diag - diag.min()) / max(1e-6, float(diag.max() - diag.min()))
    return (0.75 * diag + 0.25 * jitter).astype(np.float32)


@lru_cache(maxsize=2)
def mask_thresholds(w: int, h: int) -> np.ndarray:
    """斜め(左上→右下)に開く順番。"""
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    return (0.7 * x / max(1, w - 1) + 0.3 * y / max(1, h - 1)).astype(np.float32)


@lru_cache(maxsize=2)
def shape_thresholds(w: int, h: int) -> np.ndarray:
    """中心からの距離(円が広がる)。"""
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    d = np.hypot(x - (w - 1) / 2, y - (h - 1) / 2)
    return (d / max(1e-6, float(d.max()))).astype(np.float32)


@lru_cache(maxsize=2)
def liquid_thresholds(w: int, h: int) -> np.ndarray:
    """左から右へ、波打つ境界が進む。"""
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    u, v = x / max(1, w - 1), y / max(1, h - 1)
    wave = 0.5 + 0.5 * np.sin(2 * math.pi * (3.0 * v + 1.5 * u))
    return (0.85 * u + 0.15 * wave).astype(np.float32)


@lru_cache(maxsize=2)
def blinds_thresholds(w: int, h: int, slats: int = 8) -> np.ndarray:
    """横のスラットが上から順に開く。"""
    y = np.mgrid[0:h, 0:w][0].astype(np.float32)
    return ((y * slats / max(1, h)) % 1.0).astype(np.float32)


@lru_cache(maxsize=2)
def pixelate_thresholds(w: int, h: int, block: int = 40) -> np.ndarray:
    """ブロックごとに、決まった(再現できる)順で開く。"""
    y, x = np.mgrid[0:h, 0:w]
    bx, by = x // block, y // block
    key = ((bx.astype(np.int64) * 73856093) ^ (by.astype(np.int64) * 19349663)) % 1000
    return (key.astype(np.float32) / 1000.0)


def _reveal(a: np.ndarray, b: np.ndarray, thr: np.ndarray, p: float, feather: float) -> np.ndarray:
    """thr(0..1)が小さい画素から B に切り替わる。p=0 で完全に A、p=1 で完全に B。"""
    # 端点は丸め誤差で 254 などにならないよう、厳密に返す。
    if p <= 0.0:
        return a
    if p >= 1.0:
        return b
    wgt = np.clip((p * (1.0 + feather) - thr) / feather, 0.0, 1.0)[:, :, None]
    return np.rint(a.astype(np.float32) * (1.0 - wgt) + b.astype(np.float32) * wgt).astype(np.uint8)


_THRESHOLD_KINDS = {
    "mask": (mask_thresholds, 0.14),
    "shape": (shape_thresholds, 0.10),
    "liquid": (liquid_thresholds, 0.10),
    "blinds": (blinds_thresholds, 0.04),
    "pixelate": (pixelate_thresholds, 0.05),
}


def transition(kind: str, a: np.ndarray, b: np.ndarray, p: float) -> np.ndarray:
    p = min(1.0, max(0.0, p))
    if kind in _THRESHOLD_KINDS:
        make, feather = _THRESHOLD_KINDS[kind]
        return _reveal(a, b, make(a.shape[1], a.shape[0]), p, feather)
    if kind == "fade":
        return (a.astype(np.float32) * (1 - p) + b.astype(np.float32) * p).astype(np.uint8)
    if kind == "slide":
        w = a.shape[1]
        off = int(w * ease_out_cubic(p))
        if off <= 0:
            return a
        out = np.empty_like(a)
        out[:, : w - off] = a[:, off:]
        out[:, w - off:] = b[:, :off]
        return out
    if kind == "hex":
        thr = hex_thresholds(a.shape[1], a.shape[0])
        m = (thr < p * 1.2)[:, :, None]
        return np.where(m, b, a)
    raise ValueError(kind)


# ---------------- subtitles ----------------
def draw_subtitle(frame: np.ndarray, text: str, s: float):
    if not text:
        return
    f = font(FONT_MED, int(30 * s))
    h, w = frame.shape[:2]
    lines = wrap(text, f, int(w * 0.84))[:2]
    layer = text_layer(lines, f, (255, 255, 255), line_gap=6)
    bw, bh = layer.width + int(36 * s), layer.height + int(18 * s)
    x, y = (w - bw) // 2, int(h * 0.865) - bh // 2
    paste_rgba(frame, Image.new("RGBA", (bw, bh), (0, 0, 0, 165)), x, y)
    paste_rgba(frame, layer, x + int(18 * s), y + int(9 * s))


# ---------------- ffmpeg ----------------
def open_encoder(path: str, w: int, h: int, fps: int, audio: Optional[str]):
    cmd = ["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (w, h),
           "-r", str(fps), "-i", "-"]
    if audio:
        cmd += ["-i", audio, "-c:a", "aac", "-b:a", "128k", "-shortest"]
    cmd += ["-c:v", "libx264", "-preset", "veryfast", "-crf", "20", "-pix_fmt", "yuv420p",
            "-movflags", "+faststart", path]
    return subprocess.Popen(cmd, stdin=subprocess.PIPE)
