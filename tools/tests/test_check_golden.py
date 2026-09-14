"""Tests for tools/ci/check_golden.py, the render-hash comparison.

The gate is keyed by configuration because an exact pixel hash belongs to one
renderer: two backends drawing the same scene correctly still differ at pixel
edges, in sub-pixel precision, in filtering and in blend rounding. The tests
below pin that a second backend gets told to record its own block rather than
silently passing or reading as a regression of the first.
"""

import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "ci"))
import check_golden as golden  # noqa: E402

CONFIG = "renderer=standard2d mode=buffer size=320x240 format=RGB565 frames=60"

RENDER = f"""\
[INFO] -> SettingsRegistry.cpp:85 -> [SettingsRegistry] Registered plugin 'Rendering'
config: {CONFIG}
Rendering 2 scene(s) at 320x240 RGB565, 60 frame(s) each, mode buffer
  88381d7187f49c1b  scenes/main.scene
  47e97a77564a8bc1  scenes\\other.scene
"""

BOTH = "88381d7187f49c1b  scenes/main.scene\n47e97a77564a8bc1  scenes/other.scene\n"


def block(config, body):
    return f"# config: {config}\n{body}"


class CheckGolden(unittest.TestCase):
    def run_check(self, render, gold, extra=None):
        with tempfile.TemporaryDirectory() as tmp:
            r = Path(tmp) / "render.txt"
            g = Path(tmp) / "golden.txt"
            r.write_text(render, encoding="utf-8")
            g.write_text(gold, encoding="utf-8")
            return golden.main([str(r), str(g)] + (extra or []))

    def test_matching_hashes_pass_and_backslashes_normalise(self):
        self.assertEqual(self.run_check(RENDER, "# comment\n" + block(CONFIG, BOTH)), 0)

    def test_changed_hash_fails(self):
        gold = block(CONFIG, "0000000000000000  scenes/main.scene\n"
                             "47e97a77564a8bc1  scenes/other.scene\n")
        self.assertEqual(self.run_check(RENDER, gold), 1)

    def test_missing_or_extra_scene_fails(self):
        self.assertEqual(
            self.run_check(RENDER, block(CONFIG, "88381d7187f49c1b  scenes/main.scene\n")), 1)
        gold = block(CONFIG, BOTH + "0000000000000000  scenes/gone.scene\n")
        self.assertEqual(self.run_check(RENDER, gold), 1)

    def test_empty_render_output_fails(self):
        self.assertEqual(self.run_check("nothing rendered\n", block(CONFIG, BOTH)), 1)

    def test_render_without_a_config_line_fails(self):
        # An older editor build prints no config. Guessing which block it meant
        # is how a run gets compared against another renderer's reference.
        stripped = "\n".join(l for l in RENDER.splitlines() if not l.startswith("config:"))
        self.assertEqual(self.run_check(stripped + "\n", block(CONFIG, BOTH)), 1)

    def test_unknown_configuration_fails_rather_than_passing(self):
        # A GPU backend against a software golden. Not a regression and not a
        # pass: it has no reference yet and must be told so.
        gpu = RENDER.replace("renderer=standard2d", "renderer=gpu2d")
        self.assertEqual(self.run_check(gpu, block(CONFIG, BOTH)), 1)

    def test_each_configuration_is_checked_against_its_own_block(self):
        gpu_config = CONFIG.replace("standard2d", "gpu2d")
        gold = (block(CONFIG, BOTH) + "\n"
                + block(gpu_config, "aaaaaaaaaaaaaaaa  scenes/main.scene\n"
                                    "bbbbbbbbbbbbbbbb  scenes/other.scene\n"))
        # The software run still matches its own block...
        self.assertEqual(self.run_check(RENDER, gold), 0)
        # ...and the GPU run matches its own, with entirely different hashes.
        gpu = RENDER.replace("renderer=standard2d", "renderer=gpu2d") \
                    .replace("88381d7187f49c1b", "aaaaaaaaaaaaaaaa") \
                    .replace("47e97a77564a8bc1", "bbbbbbbbbbbbbbbb")
        self.assertEqual(self.run_check(gpu, gold), 0)

    def test_record_requires_a_reason(self):
        self.assertEqual(self.run_check(RENDER, "", ["--record"]), 2)
        self.assertEqual(self.run_check(RENDER, "", ["--record", "--reason", "   "]), 2)

    def test_record_writes_the_block_and_keeps_the_other_ones(self):
        with tempfile.TemporaryDirectory() as tmp:
            r = Path(tmp) / "render.txt"
            g = Path(tmp) / "golden.txt"
            r.write_text(RENDER, encoding="utf-8")
            gpu_config = CONFIG.replace("standard2d", "gpu2d")
            g.write_text("# header\n\n" + block(gpu_config, "aaaaaaaaaaaaaaaa  scenes/main.scene\n"),
                         encoding="utf-8")

            self.assertEqual(golden.main([str(r), str(g), "--record", "--reason", "why"]), 0)
            text = g.read_text(encoding="utf-8")

            self.assertIn("# header", text)          # header survives
            self.assertIn("recorded:", text)          # the reason is stamped
            self.assertIn("why", text)
            self.assertIn(gpu_config, text)           # the other backend is untouched
            self.assertIn("aaaaaaaaaaaaaaaa", text)
            self.assertEqual(golden.main([str(r), str(g)]), 0)


if __name__ == "__main__":
    unittest.main()


class ShippedGoldens(unittest.TestCase):
    """The golden files in tools/ci/ are the gate; nothing checked them.

    check_golden's own logic is covered above against synthetic files, so a
    golden that had been corrupted, hand-edited into an unparsable shape, or
    emptied would still have passed every test in this suite while silently
    gating nothing: a config block with no hashes compares equal to anything.
    """

    CI = Path(__file__).resolve().parents[1] / "ci"

    def goldens(self):
        found = sorted(self.CI.glob("golden-*.txt"))
        self.assertTrue(found, f"no golden files in {self.CI}")
        return found

    def test_every_golden_parses_into_at_least_one_hash(self):
        for path in self.goldens():
            with self.subTest(golden=path.name):
                blocks = golden.parse_golden(path)
                self.assertTrue(blocks, f"{path.name} has no config blocks")
                total = sum(len(scenes) for scenes in blocks.values())
                self.assertTrue(total, f"{path.name} records no hashes, so it gates nothing")

    def test_hashes_are_16_hex_digits_and_scenes_are_unique(self):
        for path in self.goldens():
            blocks = golden.parse_golden(path)
            for config, scenes in blocks.items():
                with self.subTest(golden=path.name, config=config):
                    # A scene appearing twice in one block means the later line
                    # silently won when the file was recorded by hand.
                    lines = [
                        ln.strip()
                        for ln in path.read_text(encoding="utf-8").splitlines()
                        if ln.strip() and not ln.strip().startswith("#")
                    ]
                    self.assertEqual(
                        len(scenes),
                        len({s for s in scenes}),
                        f"{path.name}: duplicate scene in block '{config}'",
                    )
                    for scene, h in scenes.items():
                        self.assertRegex(
                            h, r"^[0-9a-f]{16}$", f"{path.name}: '{scene}' hash is not 16 hex digits"
                        )
                    self.assertTrue(lines)

    def test_a_golden_matches_a_render_of_itself(self):
        """Round-trip: each block, replayed as a render capture, must pass."""
        for path in self.goldens():
            blocks = golden.parse_golden(path)
            for config, scenes in blocks.items():
                with self.subTest(golden=path.name, config=config):
                    with tempfile.TemporaryDirectory() as tmp:
                        render = Path(tmp) / "render.txt"
                        body = "".join(f"  {h}  {s}\n" for s, h in scenes.items())
                        render.write_text(f"config: {config}\n{body}", encoding="utf-8")
                        self.assertEqual(golden.main([str(render), str(path)]), 0)
