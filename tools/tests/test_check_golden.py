"""Tests for tools/ci/check_golden.py, the render-hash comparison."""

import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "ci"))
import check_golden as golden  # noqa: E402

RENDER = """\
[INFO] -> SettingsRegistry.cpp:85 -> [SettingsRegistry] Registered plugin 'Rendering'
Rendering 2 scene(s) at 320x240 RGB565, 60 frame(s) each, mode buffer
  88381d7187f49c1b  scenes/main.scene
  47e97a77564a8bc1  scenes\\other.scene
"""


class CheckGolden(unittest.TestCase):
    def run_check(self, render, gold):
        with tempfile.TemporaryDirectory() as tmp:
            r = Path(tmp) / "render.txt"
            g = Path(tmp) / "golden.txt"
            r.write_text(render, encoding="utf-8")
            g.write_text(gold, encoding="utf-8")
            return golden.main([str(r), str(g)])

    def test_matching_hashes_pass_and_backslashes_normalise(self):
        gold = "# comment\n88381d7187f49c1b  scenes/main.scene\n47e97a77564a8bc1  scenes/other.scene\n"
        self.assertEqual(self.run_check(RENDER, gold), 0)

    def test_changed_hash_fails(self):
        gold = "0000000000000000  scenes/main.scene\n47e97a77564a8bc1  scenes/other.scene\n"
        self.assertEqual(self.run_check(RENDER, gold), 1)

    def test_missing_or_extra_scene_fails(self):
        self.assertEqual(self.run_check(RENDER, "88381d7187f49c1b  scenes/main.scene\n"), 1)
        gold = "88381d7187f49c1b  scenes/main.scene\n47e97a77564a8bc1  scenes/other.scene\n0000000000000000  scenes/gone.scene\n"
        self.assertEqual(self.run_check(RENDER, gold), 1)

    def test_empty_render_output_fails(self):
        self.assertEqual(self.run_check("nothing rendered\n", "88381d7187f49c1b  scenes/main.scene\n"), 1)


if __name__ == "__main__":
    unittest.main()
