import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import numpy as np  # noqa: E402

from common import narration, subtitles  # noqa: E402
from common.tutorial_input import load_scenes, parse_frontmatter, split_sections  # noqa: E402
from python_render import engine  # noqa: E402
from python_render.render import plan_boundaries  # noqa: E402

MD = """---
title: テスト手順
tags: [a]
---

## 概要

球を作って色を塗ります。二文目です。三文目です。

## ハマりポイント

- `polymesh` にする必要がある。
- 名前は **バージョン付き** [1]。
"""


class InputTests(unittest.TestCase):
    def test_frontmatter_and_sections(self):
        meta, body = parse_frontmatter(MD)
        self.assertEqual(meta["title"], "テスト手順")
        self.assertIn("概要", split_sections(body))

    def test_scenes_without_screenshots(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "t.md")
            open(p, "w", encoding="utf-8").write(MD)
            data = load_scenes(p, None)
            kinds = [s["kind"] for s in data["scenes"]]
            self.assertEqual(kinds, ["title", "bullets"])
            title = data["scenes"][0]
            self.assertEqual(title["heading"], "テスト手順")
            self.assertTrue(title["caption"].endswith("二文目です。"))  # 先頭2文
            self.assertNotIn("`", data["scenes"][1]["bullets"][0])      # Markdown除去
            self.assertNotIn("[1]", data["scenes"][1]["bullets"][1])    # 引用記号除去


class NarrationTests(unittest.TestCase):
    def test_split_sentences(self):
        s = narration.split_sentences("一つ目です。二つ目！三つ目？\n四つ目")
        self.assertEqual(s, ["一つ目です。", "二つ目！", "三つ目？", "四つ目"])

    def test_split_long_sentence_keeps_all_text(self):
        t = "あいうえおかきくけこ、" * 12 + "終わり。"
        parts = narration.split_sentences(t, max_chars=30)
        self.assertTrue(all(len(p) <= 30 for p in parts))
        self.assertEqual("".join(parts), t)

    def test_trim_silence(self):
        pad = np.zeros(4410, np.int16)
        tone = (np.sin(np.arange(4410) / 10.0) * 8000).astype(np.int16)
        raw = np.concatenate([pad, tone, pad]).tobytes()
        trimmed = np.frombuffer(narration._trim_silence(raw), np.int16)
        self.assertLess(len(trimmed), 4410 + 2 * 1500)   # 余白はマージン(30ms)程度まで
        self.assertGreaterEqual(len(trimmed), 4410)


class SubtitleTests(unittest.TestCase):
    SEGS = [{"text": "一行目です。", "start": 0.0, "end": 1.5}, {"text": "二行目です。", "start": 2.0, "end": 3.25}]

    def test_srt(self):
        s = subtitles.to_srt(self.SEGS)
        self.assertTrue(s.startswith("1\n00:00:00,000 --> 00:00:01,500\n一行目です。"))
        self.assertIn("2\n00:00:02,000 --> 00:00:03,250", s)

    def test_vtt(self):
        v = subtitles.to_vtt(self.SEGS)
        self.assertTrue(v.startswith("WEBVTT"))
        self.assertIn("00:00:02.000 --> 00:00:03.250", v)

    def test_wrap_keeps_text(self):
        t = "あ" * 50
        self.assertEqual("".join(subtitles.wrap_cjk(t, 22)), t)

    def test_offset_measurement_against_real_audio_onsets(self):
        # 0.5s 無音 → 1s トーン → 0.5s 無音 → 1s トーン の WAV を作り、発話開始(0.5s, 2.0s)を検出できる
        import wave
        sr = 44100
        def tone(sec):
            t = np.arange(int(sec * sr)) / sr
            return (np.sin(2 * np.pi * 220 * t) * 9000).astype(np.int16)
        pcm = np.concatenate([np.zeros(sr // 2, np.int16), tone(1.0), np.zeros(sr // 2, np.int16), tone(1.0)])
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "x.wav")
            with wave.open(p, "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr); w.writeframes(pcm.tobytes())
            on = subtitles.speech_onsets(p)
            self.assertEqual(len(on), 2)
            self.assertAlmostEqual(on[0], 0.5, delta=0.03)
            self.assertAlmostEqual(on[1], 2.0, delta=0.03)
            m = subtitles.measure_offset([{"start": 0.52}, {"start": 1.95}], on)
            self.assertLess(m["max_abs"], 0.06)


class RenderPlanTests(unittest.TestCase):
    def test_boundaries_stay_inside_speech_gaps(self):
        ranges = {"0": [0.0, 4.0], "1": [4.5, 9.0], "2": [9.6, 12.0]}
        starts = plan_boundaries(ranges, 3, [])
        self.assertEqual(starts[0], 0.0)
        self.assertTrue(4.0 <= starts[1] <= 4.5)
        self.assertTrue(9.0 <= starts[2] <= 9.6)

    def test_boundaries_snap_to_beats_only_within_gap(self):
        ranges = {"0": [0.0, 4.0], "1": [4.5, 9.0], "2": [9.6, 12.0]}
        starts = plan_boundaries(ranges, 3, [1.0, 4.4, 9.1, 20.0])
        self.assertEqual(starts[1], 4.4)  # 隙間(4.0〜4.5)内の拍
        self.assertEqual(starts[2], 9.1)  # 隙間(9.0〜9.6)内の拍
        starts = plan_boundaries(ranges, 3, [1.0, 7.0])  # 隙間に拍が無い → 隙間の中央
        self.assertAlmostEqual(starts[1], 4.25)


class TransitionTests(unittest.TestCase):
    def setUp(self):
        self.a = np.zeros((90, 160, 3), np.uint8)
        self.b = np.full((90, 160, 3), 255, np.uint8)

    def test_endpoints_are_exact(self):
        for kind in engine.SUPPORTED_TRANSITIONS:
            np.testing.assert_array_equal(engine.transition(kind, self.a, self.b, 0.0), self.a, kind)
            np.testing.assert_array_equal(engine.transition(kind, self.a, self.b, 1.0), self.b, kind)

    def test_every_kind_is_a_pure_function_of_time(self):
        # 1コマずつ描く方式の前提: 同じ p なら何度呼んでも同じ絵になる
        for kind in engine.SUPPORTED_TRANSITIONS:
            first = engine.transition(kind, self.a, self.b, 0.4)
            np.testing.assert_array_equal(first, engine.transition(kind, self.a, self.b, 0.4), kind)

    def test_midway_has_both_pictures_for_every_kind(self):
        for kind in ("fade", "slide", "hex", "mask", "shape", "liquid", "blinds", "pixelate"):
            mid = engine.transition(kind, self.a, self.b, 0.5)
            self.assertTrue((mid < 255).any() and (mid > 0).any(), kind)
            self.assertFalse(np.array_equal(mid, self.a) or np.array_equal(mid, self.b), kind)

    def test_shape_opens_from_the_centre_and_mask_from_the_top_left(self):
        h, w = self.a.shape[:2]
        shape = engine.transition("shape", self.a, self.b, 0.3)
        self.assertEqual(int(shape[h // 2, w // 2, 0]), 255)  # 中心は先に B
        self.assertEqual(int(shape[0, 0, 0]), 0)              # 角はまだ A
        mask = engine.transition("mask", self.a, self.b, 0.3)
        self.assertGreater(int(mask[0, 0, 0]), int(mask[h - 1, w - 1, 0]))

    def test_plan_allows_at_most_three_kinds(self):
        ok = engine.validate_transition_plan({"intro": "mask", "steps": "shape", "outro": "mask"})
        self.assertEqual(ok["steps"], "shape")
        # 章が3つしかないので4種類には本来ならないが、上限そのものを検証する
        with self.assertRaises(ValueError):
            engine.validate_transition_plan({"intro": "fade", "steps": "mask", "outro": "shape", "extra": "hex"})
        with self.assertRaises(ValueError):
            engine.validate_transition_plan({"intro": "swirl"})

    def test_parse_plan_overrides_the_default(self):
        plan = engine.parse_transition_plan("steps=mask, outro=liquid")
        self.assertEqual(plan, {"intro": "fade", "steps": "mask", "outro": "liquid"})
        self.assertEqual(engine.parse_transition_plan(""), engine.CHAPTER_TRANSITION)
        with self.assertRaises(ValueError):
            engine.parse_transition_plan("steps")

    def test_three_kinds_only_and_fixed_per_chapter(self):
        self.assertLessEqual(len(set(engine.CHAPTER_TRANSITION.values())), 3)
        self.assertEqual(set(engine.CHAPTER_TRANSITION), {"intro", "steps", "outro"})

    def test_midpoint_mixes(self):
        mid = engine.transition("fade", self.a, self.b, 0.5)
        self.assertTrue(120 <= int(mid[0, 0, 0]) <= 135)


class HyperframesTransitionTests(unittest.TestCase):
    """HyperFrames版は clip-path / transform で書けるものだけ使える。"""

    def test_rejects_kinds_the_html_version_cannot_draw(self):
        from hyperframes import build_project
        with self.assertRaises(ValueError):
            build_project.build("x", None, "x", "x", transitions={"steps": "liquid"})
        with self.assertRaises(ValueError):  # 3種を超える指定は共通の検証で弾かれる
            build_project.build("x", None, "x", "x",
                                transitions={"intro": "fade", "steps": "mask", "outro": "shape", "extra": "hex"})

    def test_new_transitions_become_clip_paths(self):
        from hyperframes import build_project
        if not os.path.exists(os.path.join(build_project.FONT_DIR, "NotoSansJP-Bold.otf")):
            self.skipTest("Noto Sans JP is not installed (build copies the fonts)")
        import wave
        with tempfile.TemporaryDirectory() as d:
            md = os.path.join(d, "t.md")
            with open(md, "w", encoding="utf-8") as f:
                f.write(MD)
            os.makedirs(os.path.join(d, "narr"))
            wav = os.path.join(d, "narr", "narration.wav")
            with wave.open(wav, "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(8000)
                w.writeframes(b"\x00\x00" * 8000)
            with open(os.path.join(d, "narr", "timings.json"), "w", encoding="utf-8") as f:
                json.dump({"wav": wav, "duration": 5.5,
                           "segments": [{"start": 0.1, "end": 2.0, "text": "球を作ります。"},
                                        {"start": 3.1, "end": 5.0, "text": "名前に注意。"}],
                           "scene_ranges": {"0": [0.1, 2.0], "1": [3.1, 5.0]}}, f)
            for kind, expect in (("mask", build_project.MASK_TO), ("shape", build_project.SHAPE_TO)):
                out = os.path.join(d, "proj_" + kind)
                build_project.build(md, None, d, out, transitions={"intro": kind, "steps": kind, "outro": kind})
                with open(os.path.join(out, "index.html"), encoding="utf-8") as f:
                    page = f.read()
                self.assertIn(expect, page, kind)


class EasingTests(unittest.TestCase):
    def test_overshoot(self):
        vals = [engine.ease_out_back(t / 100) for t in range(101)]
        self.assertAlmostEqual(vals[0], 0.0, places=6)
        self.assertAlmostEqual(vals[-1], 1.0, places=6)
        self.assertGreater(max(vals), 1.05)  # 行き過ぎてから戻る


class WrapTests(unittest.TestCase):
    def test_does_not_split_ascii_words(self):
        f = engine.font(engine.FONT_MED, 28)
        lines = engine.wrap("「Attribute Wrangle」ノード(attribwrangle)を作り、名前を color にする", f, 300)
        joined = "".join(lines)
        for word in ("attribwrangle", "Attribute"):
            self.assertTrue(any(word in l for l in lines), word)
        self.assertEqual(joined.replace(" ", ""), "「Attribute Wrangle」ノード(attribwrangle)を作り、名前を color にする".replace(" ", ""))


if __name__ == "__main__":
    unittest.main()
