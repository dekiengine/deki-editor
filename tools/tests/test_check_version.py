"""Tests for tools/ci/check_version.py.

The changelog-section check had no test and did not match the format the
changelog actually uses, so a tag build would have failed on every release.
"""
import importlib.util
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("check_version", REPO / "tools" / "ci" / "check_version.py")
check_version = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(check_version)


class ChangelogSection(unittest.TestCase):
    def setUp(self):
        self.original = check_version.CHANGELOG

    def tearDown(self):
        check_version.CHANGELOG = self.original

    def write(self, text):
        import tempfile
        self.tmp = tempfile.NamedTemporaryFile("w", suffix=".md", delete=False, encoding="utf-8")
        self.tmp.write(text)
        self.tmp.close()
        check_version.CHANGELOG = Path(self.tmp.name)

    def test_heading_with_a_date_counts(self):
        # The shape every entry in the real changelog uses.
        self.write("# Changelog\n\n## 0.16.0 - 2026-09-16\n\n### Changed\n- something\n")
        self.assertTrue(check_version.changelog_has_section("0.16.0"))

    def test_bare_heading_counts(self):
        self.write("# Changelog\n\n## 0.16.0\n\n- something\n")
        self.assertTrue(check_version.changelog_has_section("0.16.0"))

    def test_missing_version_does_not_count(self):
        self.write("# Changelog\n\n## 0.15.0 - 2026-09-14\n")
        self.assertFalse(check_version.changelog_has_section("0.16.0"))

    def test_a_longer_version_is_not_a_match(self):
        # "0.16.0" must not be satisfied by a section for "0.16.01".
        self.write("# Changelog\n\n## 0.16.01 - 2026-09-16\n")
        self.assertFalse(check_version.changelog_has_section("0.16.0"))

    def test_unreleased_is_not_a_match(self):
        self.write("# Changelog\n\n## Unreleased\n\n## 0.15.0 - 2026-09-14\n")
        self.assertFalse(check_version.changelog_has_section("0.16.0"))

    def test_the_real_changelog_has_the_current_version(self):
        # Guards the release itself: whatever the editor version is, the
        # changelog must carry a section for it.
        version = check_version.project_version(check_version.EDITOR_CMAKE, "DekiEditor")
        self.assertIsNotNone(version)
        self.assertTrue(check_version.changelog_has_section(version),
                        f"CHANGELOG.md has no section for {version}")


if __name__ == "__main__":
    unittest.main()
