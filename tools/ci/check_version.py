#!/usr/bin/env python3
"""Fail when the editor, the engine and (optionally) a git tag disagree on the version.

The rule (COMPATIBILITY.md): the engine carries the editor's version number,
packages name it as `minEngine`, and the release tag equals it. This script is
the check; CI runs it on every build and the release workflow passes the tag.

    python tools/ci/check_version.py            # editor == engine
    python tools/ci/check_version.py --tag 0.10.0
    python tools/ci/check_version.py --print    # just print the editor version

Exit status 0 when consistent, 1 otherwise, with one line per complaint.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
EDITOR_CMAKE = REPO / "CMakeLists.txt"
ENGINE_CMAKE = REPO / "deki-engine" / "CMakeLists.txt"
CHANGELOG = REPO / "CHANGELOG.md"

SEMVER = r"\d+\.\d+\.\d+"


def project_version(cmake: Path, name: str) -> str | None:
    """The VERSION of `project(<name> VERSION x.y.z ...)` in a CMakeLists."""
    text = cmake.read_text(encoding="utf-8")
    m = re.search(r"project\(\s*" + re.escape(name) + r"\s+VERSION\s+(" + SEMVER + r")", text)
    return m.group(1) if m else None


def changelog_has_section(version: str) -> bool:
    if not CHANGELOG.exists():
        return False
    return re.search(r"^## " + re.escape(version) + r"\s*$", CHANGELOG.read_text(encoding="utf-8"), re.M) is not None


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", help="git tag being released; must equal the editor version (bare semver)")
    ap.add_argument("--print", action="store_true", help="print the editor version and exit")
    args = ap.parse_args(argv)

    editor = project_version(EDITOR_CMAKE, "DekiEditor")
    engine = project_version(ENGINE_CMAKE, "deki-engine")
    problems: list[str] = []

    if editor is None:
        problems.append(f"{EDITOR_CMAKE}: no `project(DekiEditor VERSION x.y.z)` found")
    if engine is None:
        problems.append(f"{ENGINE_CMAKE}: no `project(deki-engine VERSION x.y.z)` found")

    if args.print:
        print(editor or "")
        return 0 if editor else 1

    if editor and engine and editor != engine:
        problems.append(f"editor is {editor} but engine is {engine}: the engine carries the editor's version (COMPATIBILITY.md)")

    if args.tag is not None:
        tag = args.tag.strip()
        if tag.startswith("refs/tags/"):
            tag = tag[len("refs/tags/"):]
        if not re.fullmatch(SEMVER, tag):
            problems.append(f"tag '{tag}' is not bare semver (x.y.z); package checkouts use the tag verbatim")
        elif editor and tag != editor:
            problems.append(f"tag {tag} but editor version is {editor}: bump project(DekiEditor VERSION) and deki-engine's before tagging")
        if editor and not changelog_has_section(editor):
            problems.append(f"CHANGELOG.md has no '## {editor}' section for the release")

    for p in problems:
        print(f"check_version: {p}", file=sys.stderr)
    if not problems:
        print(f"check_version: editor {editor}, engine {engine}" + (f", tag {args.tag}" if args.tag else "") + ": consistent")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
