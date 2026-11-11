"""tools/verify_export.py: 良い動画は合格し、**悪い動画は実際に不合格になる**(QL5 の Go の条件)。

ffmpeg で、1 つずつ欠陥を持つ短い動画を作って検査する。ffmpeg / ffprobe が無い環境では、すべて skip。
実行: python -m unittest discover -s tools/tests -v
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import verify_export as ve  # noqa: E402

HAVE_FFMPEG = bool(shutil.which("ffmpeg") and shutil.which("ffprobe"))
SPEC = json.load(open(ve.DEFAULT_SPEC, encoding="utf-8"))
GOOD_COLOR = "0x10140f"  # エンジンが宣言した左パネルの地(video_spec.json の目標色)
DUR = 6


def ffmpeg(*args: str) -> None:
    done = subprocess.run(["ffmpeg", "-y", "-v", "error", *args], capture_output=True, text=True)
    assert done.returncode == 0, done.stderr


def make_video(path: str, *, color: str = GOOD_COLOR, video_sec: float = DUR, audio: str = "sine", audio_sec: float = DUR,
               volume: float = 0.3, box: bool = True, fps: int = 15) -> str:
    """color: 映像の地の色。box: 明るい長方形を足す(実際の動画のスライドは、地のほかに明るい部分を持つ。全面が暗い単色は「黒」と判定される)。
    audio: sine(正弦波)/ silent(無音)/ none(音声なし)。"""
    inputs = ["-f", "lavfi", "-i", f"color=c={color}:s=1280x720:r={fps}:d={video_sec}"]
    vfilter = ["-vf", "drawbox=x=500:y=200:w=600:h=300:color=0xf5f2e8:t=fill"] if box else []
    if audio == "sine":
        inputs += ["-f", "lavfi", "-i", f"sine=frequency=440:sample_rate=44100:duration={audio_sec}"]
        afilter = ["-af", f"volume={volume}"]
    elif audio == "silent":
        inputs += ["-f", "lavfi", "-i", f"anullsrc=r=44100:cl=mono:d={audio_sec}"]
        afilter = []
    else:
        afilter = []
    codec = ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-preset", "ultrafast"]
    if audio != "none":
        codec += ["-c:a", "aac", "-b:a", "128k"]
    else:
        codec += ["-an"]
    ffmpeg(*inputs, *vfilter, *afilter, *codec, path)
    return path


def statuses(checks):
    return {c.name: c.status for c in checks}


@unittest.skipUnless(HAVE_FFMPEG, "ffmpeg / ffprobe が必要")
class VerifyExportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="verify_export_")
        j = lambda n: os.path.join(cls.tmp, n)
        cls.good = make_video(j("good.mp4"), video_sec=DUR + 1.5)  # ナレーション 6 秒 + エンジンが宣言した余白 1.5 秒
        cls.legacy_fps = make_video(j("legacy_fps.mp4"), fps=30)
        cls.wrong_fps = make_video(j("wrong_fps.mp4"), fps=24)
        cls.no_tail = make_video(j("no_tail.mp4"))  # 映像 = 音声(余白なし)も、範囲の内側
        cls.audio_short = make_video(j("audio_short.mp4"), audio_sec=3.0)  # 映像 6 秒・音声 3 秒
        cls.audio_long = make_video(j("audio_long.mp4"), audio_sec=9.0)
        cls.wrong_color = make_video(j("wrong_color.mp4"), color="0xd9702a")  # ブランドのオレンジなど、別の色
        cls.black = make_video(j("black.mp4"), color="black", box=False)
        cls.clipped = make_video(j("clipped.mp4"), volume=12.0)  # 大きすぎて、波形が上限に張り付く
        cls.silent = make_video(j("silent.mp4"), audio="silent")
        cls.no_audio = make_video(j("no_audio.mp4"), audio="none")
        cls.twin_same = make_video(j("twin_same.mp4"))
        cls.twin_other = make_video(j("twin_other.mp4"), color="0x2b3226")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_checks(self, path, **kw):
        return ve.verify(path, SPEC, **kw)

    # ---- 良い動画
    def test_good_video_passes_everything(self):
        s = statuses(self.run_checks(self.good))
        self.assertEqual({k: v for k, v in s.items() if v == ve.FAIL}, {})
        for name in ("streams", "av-duration", "colors", "black-frames", "audio-levels"):
            self.assertEqual(s[name], ve.PASS, name)

    # ---- 悪い動画は、実際に不合格になる
    def test_video_longer_than_the_declared_tail_padding_fails(self):
        # 映像 6 秒・音声 3 秒 = 余白(1.5 秒)を 1.5 秒超えて、声が終わった後も映像が続く
        c = next(c for c in self.run_checks(self.audio_short) if c.name == "av-duration")
        self.assertEqual(c.status, ve.FAIL)
        self.assertIn("余白", c.detail)

    def test_audio_longer_than_video_fails(self):
        c = next(c for c in self.run_checks(self.audio_long) if c.name == "av-duration")
        self.assertEqual(c.status, ve.FAIL)
        self.assertIn("音声が映像より長い", c.detail)

    def test_declared_tail_padding_passes_and_no_padding_passes(self):
        for path in (self.good, self.no_tail):  # 映像 = 音声 + 1.5 秒 / 映像 = 音声
            self.assertEqual(next(c for c in self.run_checks(path) if c.name == "av-duration").status, ve.PASS, path)

    def test_boundaries_of_the_duration_rule(self):
        info = ve.probe(self.good)
        v = info["video_duration"]
        for audio_gap, expected in ((1.5, ve.PASS), (1.59, ve.PASS), (1.7, ve.FAIL), (0.0, ve.PASS), (-0.05, ve.PASS), (-0.2, ve.FAIL)):
            info["audio_duration"] = v - audio_gap  # gap = 映像 − 音声
            self.assertEqual(ve.check_av_duration(info, SPEC).status, expected, f"映像 − 音声 = {audio_gap}")

    def test_fps(self):
        s = lambda path: next(c for c in self.run_checks(path) if c.name == "streams").status
        self.assertEqual(s(self.good), ve.PASS)
        self.assertEqual(s(self.legacy_fps), ve.WARN)  # 旧版(30fps)の書き出しは警告
        self.assertEqual(s(self.wrong_fps), ve.FAIL)

    def test_wrong_brand_color_fails(self):
        c = next(c for c in self.run_checks(self.wrong_color) if c.name == "colors")
        self.assertEqual(c.status, ve.FAIL)
        self.assertIn("目標", c.detail)

    def test_color_inside_tolerance_passes_but_a_visible_shift_fails(self):
        # エンコードで #10140f が #10120d 前後にずれる程度は合格。見た目に違う色は不合格
        self.assertLess(ve.delta_e((0x10, 0x12, 0x0d), (0x10, 0x14, 0x0f)), 3)
        self.assertGreater(ve.delta_e((0x1c, 0x1c, 0x1c), (0x10, 0x14, 0x0f)), 3)

    def test_black_video_fails(self):
        s = statuses(self.run_checks(self.black))
        self.assertEqual(s["black-frames"], ve.FAIL)

    def test_clipped_audio_fails(self):
        c = next(c for c in self.run_checks(self.clipped) if c.name == "audio-levels")
        self.assertEqual(c.status, ve.FAIL)
        self.assertIn("音割れ", c.detail)

    def test_silent_audio_fails(self):
        c = next(c for c in self.run_checks(self.silent) if c.name == "audio-levels")
        self.assertEqual(c.status, ve.FAIL)
        self.assertIn("無音", c.detail)

    def test_missing_audio_stream_fails(self):
        s = statuses(self.run_checks(self.no_audio))
        self.assertEqual(s["streams"], ve.FAIL)

    def test_wrong_resolution_fails(self):
        info = ve.probe(self.good)
        spec = {**SPEC, "frame": {"width": 1920, "height": 1080, "fps": 15}}
        self.assertEqual(ve.check_streams(info, spec).status, ve.FAIL)

    # ---- 字幕
    def write(self, name, text):
        p = os.path.join(self.tmp, name)
        open(p, "w", encoding="utf-8").write(text)
        return p

    def test_subtitles_within_duration_pass_and_vtt_is_read(self):
        srt = self.write("ok.srt", "1\n00:00:00,500 --> 00:00:02,000\nあ\n\n2\n00:00:02,500 --> 00:00:05,500\nい\n")
        vtt = self.write("ok.vtt", "WEBVTT\n\n00:00:00.500 --> 00:00:02.000\nあ\n\n00:00:02.500 --> 00:00:05.500\nい\n")
        for path in (srt, vtt):
            self.assertEqual(ve.check_subtitles(path, DUR, SPEC).status, ve.PASS, path)

    def test_subtitle_ending_after_the_video_fails(self):
        srt = self.write("late.srt", "1\n00:00:00,500 --> 00:00:02,000\nあ\n\n2\n00:00:05,000 --> 00:00:09,000\nい\n")
        c = ve.check_subtitles(srt, DUR, SPEC)
        self.assertEqual(c.status, ve.FAIL)
        self.assertIn("超える", c.detail)

    def test_subtitle_going_backwards_fails(self):
        srt = self.write("back.srt", "1\n00:00:03,000 --> 00:00:04,000\nあ\n\n2\n00:00:01,000 --> 00:00:02,000\nい\n")
        self.assertEqual(ve.check_subtitles(srt, DUR, SPEC).status, ve.FAIL)

    def test_missing_subtitles_are_skipped_not_failed(self):
        self.assertEqual(ve.check_subtitles(None, DUR, SPEC).status, ve.SKIP)

    # ---- 章
    def test_chapters_contiguous_pass_and_gap_fails(self):
        ok = {"chapters": [{"startSec": 0, "durationSec": 2}, {"startSec": 2, "durationSec": 4}]}
        self.assertEqual(ve.check_chapters(ok, DUR, SPEC).status, ve.PASS)
        gap = {"chapters": [{"startSec": 0, "durationSec": 2}, {"startSec": 3, "durationSec": 3}]}
        self.assertEqual(ve.check_chapters(gap, DUR, SPEC).status, ve.FAIL)
        short = {"chapters": [{"startSec": 0, "durationSec": 2}]}
        self.assertEqual(ve.check_chapters(short, DUR, SPEC).status, ve.FAIL)
        self.assertEqual(ve.check_chapters({}, DUR, SPEC).status, ve.SKIP)  # 古い書き出し

    # ---- 決定性
    def test_twin_identical_passes_and_different_fails(self):
        self.assertEqual(ve.check_twin(self.good, self.twin_same, DUR, SPEC).status, ve.PASS)
        self.assertEqual(ve.check_twin(self.good, self.twin_other, DUR, SPEC).status, ve.FAIL)

    def test_twin_with_different_length_fails(self):
        c = ve.check_twin(self.good, self.good, DUR + 3, SPEC)  # 長さの申告が違う = 同じ入力の書き出しではない
        self.assertEqual(c.status, ve.FAIL)

    # ---- コンタクトシート
    def test_contact_sheet_is_written_with_a_row_per_beat(self):
        out = os.path.join(self.tmp, "sheet.png")
        meta = {"chapters": [{"startSec": 0, "durationSec": 2}, {"startSec": 2, "durationSec": 2}, {"startSec": 4, "durationSec": 2}]}
        c = ve.contact_sheet(self.good, DUR + 1.5, meta, out)
        self.assertEqual(c.status, ve.PASS)
        from PIL import Image
        w, h = Image.open(out).size
        self.assertEqual(c.data["beats"], 3)
        self.assertGreater(h, 3 * 180)  # 3 拍ぶんの行
        self.assertGreater(w, 3 * 320)  # 前・拍・後の 3 列

    # ---- 入口(フォルダ・終了コード)
    def test_cli_exit_code_and_folder_layout(self):
        folder = os.path.join(self.tmp, "vid")
        os.makedirs(folder, exist_ok=True)
        shutil.copy(self.good, os.path.join(folder, "video.mp4"))
        json.dump({"chapters": [{"startSec": 0, "durationSec": DUR + 1.5}]}, open(os.path.join(folder, "metadata.json"), "w"))
        self.assertEqual(ve.main([folder]), 0)
        shutil.copy(self.audio_short, os.path.join(folder, "video.mp4"))
        self.assertEqual(ve.main([folder]), 1)

    def test_missing_video_fails_cleanly(self):
        c = self.run_checks(os.path.join(self.tmp, "nope"))
        self.assertEqual(c[0].status, ve.FAIL)


if __name__ == "__main__":
    unittest.main()
