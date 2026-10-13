#!/usr/bin/env python3
"""Clone Deki packages from the registry, at master, into one directory.

    python tools/ci/clone_packages.py <packages dir>
    python tools/ci/clone_packages.py <packages dir> --for <package id>

The package tests and compile checks (tests/CMakeLists.txt) find packages in
DEKI_PACKAGES_DIR, ../packages by default, and a package compiles only with the
packages it requires beside it. The registry's index.json is the list of
packages, so one published there is tested here without this script changing.
A directory that is already there is left as it is.

With --for, only what that package needs: everything it requires, and what
its features require, followed through each of those in turn. The package
itself must already be in <packages dir> (its own CI checks it out at the
commit under test); the rest are cloned at master.

Exit 0 when every package is in place, 1 otherwise.
"""

from __future__ import annotations

import json
import subprocess
import sys
import urllib.request
from pathlib import Path

INDEX_URL = "https://raw.githubusercontent.com/dekiengine/registry/master/index.json"


def clone(pkg_id: str, repo: str, packages: Path) -> bool:
    dest = packages / pkg_id
    if dest.exists():
        print(f"{pkg_id}: already there")
        return True
    url = f"https://github.com/{repo}"
    if subprocess.run(["git", "clone", "--quiet", "--depth", "1", url, str(dest)]).returncode != 0:
        return False
    print(f"{pkg_id}: cloned")
    return True


def requires_of(package_dir: Path) -> list[str]:
    """What a package requires, its features' requirements included."""
    manifest = package_dir / "package.json"
    if not manifest.exists():
        return []
    data = json.loads(manifest.read_text(encoding="utf-8"))
    needed = list(data.get("requires", []))
    for feature in data.get("features", []):
        needed += feature.get("requires", [])
    return needed


def main() -> int:
    args = sys.argv[1:]
    only_for = None
    if "--for" in args:
        i = args.index("--for")
        if i + 1 >= len(args):
            print(__doc__)
            return 1
        only_for = args[i + 1]
        del args[i : i + 2]
    if len(args) != 1:
        print(__doc__)
        return 1
    packages = Path(args[0]).resolve()
    packages.mkdir(parents=True, exist_ok=True)

    with urllib.request.urlopen(INDEX_URL, timeout=60) as response:
        repos = {p["id"]: p["repo"] for p in json.load(response)["packages"]}

    failed = []
    if only_for is None:
        for pkg_id in sorted(repos):
            if not clone(pkg_id, repos[pkg_id], packages):
                failed.append(pkg_id)
    else:
        if not (packages / only_for).exists():
            print(f"{only_for} is not in {packages}; check it out there first")
            return 1
        seen = {only_for}
        pending = requires_of(packages / only_for)
        while pending:
            pkg_id = pending.pop()
            if pkg_id in seen:
                continue
            seen.add(pkg_id)
            if pkg_id not in repos:
                print(f"{pkg_id}: required but not in the registry")
                failed.append(pkg_id)
                continue
            if not clone(pkg_id, repos[pkg_id], packages):
                failed.append(pkg_id)
                continue
            pending += requires_of(packages / pkg_id)

    if failed:
        print(f"could not clone: {', '.join(failed)}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
