# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# scripts/perf_baseline.py on a synthetic run (hand-computed expectations; no game data):
#   /opt/homebrew/bin/python3 -B -I tests/test_perf_baseline.py
import importlib.util
import os
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("perf_baseline", os.path.join(HERE, "..", "scripts", "perf_baseline.py"))
pb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pb)


def swap(n, draws, resolves, bytes_):
    return (f"[gpu] swap {n} presented offscreen; since the last swap: {draws} draw(s) rendered, 48 no-write draw(s) recorded as "
            f"nothing (draw_record.h kNoWriteDraws), {resolves} resolve(s), 4 shader load(s); 0 bytes of the slot's ring used, "
            f"0 ring flush(es); textures: 1 upload(s), {bytes_} bytes since the last swap; 0 image(s) (0 MB)\n")


def audio(wall):
    return f"[audio] periodic: callbacks 1, submitted 1; output {wall} s, wall {wall} s, vblanks/60 {wall} s since the first pull; mute\n"


class PerfBaseline(unittest.TestCase):
    def build(self, tmp):
        # Intro: swaps 1-4, 5 draws each, 20 swaps/s (stamps at swap 2 / 0.1 s and swap 4 / 0.2 s).
        # Menu: swaps 5-12, 100 draws, 2 resolves, 3,000,000 bytes; stamps at swap 8 / 1.0 s and swap 12 / 2.0 s -> 4 swaps/s.
        # The window from swap 4 (intro) to swap 8 (menu) spans both phases and is left out. The target is logged after
        # swap 4, so it is created in frame 5 (the menu).
        lines = [swap(1, 5, 0, 1000000), swap(2, 5, 0, 1000000), audio(0.1), swap(3, 5, 0, 1000000), swap(4, 5, 0, 1000000), audio(0.2)]
        lines.append("[edram] host target for key 108000: 640x1024, 4 sample(s), format 130\n")
        for n in range(5, 13):
            lines.append(swap(n, 100, 2, 3000000))
            if n == 8:
                lines.append(audio(1.0))
        lines.append(audio(2.0))
        lines.append("[stop 11] swap 12: front buffer 1B441000, 1280x720\n")
        run = os.path.join(tmp, "ev", "run")
        os.makedirs(run)
        open(os.path.join(run, "run1.err"), "w").writelines(lines)
        open(os.path.join(tmp, "ev", "commit.txt"), "w").write("abc1234\n")
        open(os.path.join(tmp, "ev", "launcher.txt"), "w").write("run 1: layer inserted, no validation message\n")
        return run

    def test_phases_rates_and_counters(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = self.build(tmp)
            text = open(pb.summarize(run, "run1.err")[0]).read()
            self.assertIn("| Swaps | 1-4 | 5-12 | unavailable |", text)
            # intro: (4 - 2) / (0.2 - 0.1) = 20 FPS, 50 ms; menu: (12 - 8) / (2.0 - 1.0) = 4 FPS, 250 ms.
            self.assertIn("| Presented FPS (mean of ~2 s windows) | 20.00 | 4.00 | unavailable |", text)
            self.assertIn("| Frame time ms (mean) | 50.0 | 250.0 | unavailable |", text)
            self.assertIn("| Frame time ms (worst window) | 50.0 | 250.0 | unavailable |", text)
            self.assertIn("| Draws per frame (mean / max) | 5.0 / 5 | 100.0 / 100 | unavailable |", text)
            self.assertIn("| Texture upload MB per frame (mean / max) | 1.00 / 1.00 | 3.00 / 3.00 | unavailable |", text)
            self.assertIn("| GPU busy ms per frame | unavailable (not instrumented) |", text)
            self.assertIn("| Screen | legal screen + intro video | front end (welcome / menus) | not reached |", text)
            self.assertIn("108000: 640x1024, 4x", text.split("Render targets created")[1].split("|")[2])  # created in the menu column
            self.assertIn("commit abc1234", text)
            self.assertIn("inserted, 0 messages", text)

    def test_index_separates_phases(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = self.build(tmp)
            out = os.path.join(tmp, "index.md")
            self.assertEqual(pb.main(["x", "--index", out, run]), 0)
            text = open(out).read()
            intro, rest = text.split("## intro")[1].split("## menu")
            menu, race = rest.split("## race")
            self.assertIn("| 1-4 | 20.00 | 50.0 |", intro)
            self.assertIn("| 5-12 | 4.00 | 250.0 |", menu)
            self.assertNotIn("| 5-12 |", intro)
            self.assertIn("No run has reached this phase: unavailable.", race)

    def test_control_no_menu(self):
        # CONTROL: a run that never exceeds 20 draws is all intro (no menu column).
        self.assertEqual(pb.phases([(1, 5, 0, 0, 0), (2, 20, 0, 0, 0)]), [("intro", 1, 2)])
        self.assertEqual(pb.phases([(1, 5, 0, 0, 0), (2, 21, 0, 0, 0)]), [("intro", 1, 1), ("menu", 2, 2)])


if __name__ == "__main__":
    unittest.main(verbosity=2)
