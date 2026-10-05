#!/usr/bin/env python3
"""Give a CI copy of deki-demo every package it uses, at master.

    python tools/ci/link_demo_packages.py <deki-demo> <packages dir>

deki-demo is checked against the editor and engine on master, so its packages
are too: the registry's released versions do not build against an unreleased
engine. Each package the demo declares, and each package those require, is
cloned from github.com/dekiengine into <packages dir> and linked into
<deki-demo>/packages/, as the workspace links them locally.

Exit 0 when every package is in place, 1 otherwise.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Callable

ORG_URL = "https://github.com/dekiengine/"


def required_packages(declared: list[str], requires_of: Callable[[str], list[str]]) -> list[str]:
    """Every package in `declared` and everything they require, in the order found."""
    found: list[str] = []
    queue = list(declared)
    while queue:
        pkg = queue.pop(0)
        if pkg in found:
            continue
        found.append(pkg)
        queue.extend(requires_of(pkg))
    return found


def declared_packages(demo: Path) -> list[str]:
    manifest = json.loads((demo / "deki-packages.json").read_text(encoding="utf-8"))
    return sorted(manifest.get("packages", {}))


def clone(pkg: str, packages: Path) -> Path:
    dest = packages / pkg
    if not dest.exists():
        subprocess.run(["git", "clone", "--quiet", "--depth", "1", ORG_URL + pkg, str(dest)], check=True)
    return dest


def requires_in(package_dir: Path) -> list[str]:
    manifest = package_dir / "package.json"
    if not manifest.exists():
        return []
    return list(json.loads(manifest.read_text(encoding="utf-8")).get("requires", []))


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    demo = Path(sys.argv[1]).resolve()
    packages = Path(sys.argv[2]).resolve()
    packages.mkdir(parents=True, exist_ok=True)
    (demo / "packages").mkdir(exist_ok=True)

    try:
        needed = required_packages(declared_packages(demo), lambda pkg: requires_in(clone(pkg, packages)))
    except subprocess.CalledProcessError as e:
        print(f"could not clone: {e}")
        return 1

    for pkg in needed:
        link = demo / "packages" / pkg
        if link.is_symlink() or link.exists():
            continue
        os.symlink(packages / pkg, link, target_is_directory=True)
        print(f"{pkg}: linked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
