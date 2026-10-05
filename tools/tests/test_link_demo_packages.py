"""Tests for tools/ci/link_demo_packages.py: which packages a demo needs."""
import importlib.util
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("link_demo_packages",
                                              REPO / "tools" / "ci" / "link_demo_packages.py")
link_demo_packages = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(link_demo_packages)


class RequiredPackages(unittest.TestCase):
    REQUIRES = {
        "deki-lilygo-boards": ["deki-esp32-integration", "deki-input"],
        "deki-esp32-integration": ["deki-wifi"],
        "deki-input": [],
        "deki-wifi": [],
        "deki-2d": ["deki-rendering"],
        "deki-rendering": [],
    }

    def needed(self, declared):
        return link_demo_packages.required_packages(declared, lambda pkg: self.REQUIRES[pkg])

    def test_requirements_are_followed(self):
        self.assertEqual(set(self.needed(["deki-lilygo-boards"])),
                         {"deki-lilygo-boards", "deki-esp32-integration", "deki-input", "deki-wifi"})

    def test_each_package_appears_once(self):
        needed = self.needed(["deki-2d", "deki-rendering", "deki-lilygo-boards", "deki-input"])
        self.assertEqual(len(needed), len(set(needed)))

    def test_nothing_declared_needs_nothing(self):
        self.assertEqual(self.needed([]), [])


if __name__ == "__main__":
    unittest.main()
