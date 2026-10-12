import os, sys, tempfile, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from common import beats


class BeatTests(unittest.TestCase):
    def test_known_bpm(self):
        for bpm in (90, 100, 120, 128, 140, 150):
            with tempfile.TemporaryDirectory() as d:
                p = beats.synth_click_track(os.path.join(d, "t.wav"), bpm, seconds=24)
                r = beats.detect(p)
                self.assertAlmostEqual(r["bpm"], bpm, delta=1.5, msg=f"bpm={bpm} got {r['bpm']}")
                # 拍の間隔も一定(±30ms)
                gaps = [b - a for a, b in zip(r["beats"], r["beats"][1:])]
                self.assertLess(max(abs(g - 60.0 / bpm) for g in gaps), 0.03)

    def test_snap(self):
        self.assertEqual(beats.snap_to_beats([1.1, 5.0], [0.0, 1.0, 2.0]), [1.0, 5.0])


if __name__ == "__main__":
    unittest.main()
