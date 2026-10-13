#!/usr/bin/env python3
"""Clone every package in the Deki registry, at master, into one directory.

    python tools/ci/clone_packages.py <packages dir>

The package tests and compile checks (tests/CMakeLists.txt) find packages in
DEKI_PACKAGES_DIR, ../packages by default, and a package compiles only with the
packages it requires beside it. The registry's index.json is the list of
packages, so one published there is tested here without this script changing.
A directory that is already there is left as it is.

Exit 0 when every package is in place, 1 otherwise.
"""

from __future__ import annotations

import json
import subprocess
import sys
import urllib.request
from pathlib import Path

INDEX_URL = "https://raw.githubusercontent.com/dekiengine/registry/master/index.json"


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    packages = Path(sys.argv[1]).resolve()
    packages.mkdir(parents=True, exist_ok=True)

    with urllib.request.urlopen(INDEX_URL, timeout=60) as response:
        index = json.load(response)

    failed = []
    for pkg in sorted(index["packages"], key=lambda p: p["id"]):
        dest = packages / pkg["id"]
        if dest.exists():
            print(f"{pkg['id']}: already there")
            continue
        url = f"https://github.com/{pkg['repo']}"
        result = subprocess.run(["git", "clone", "--quiet", "--depth", "1", url, str(dest)])
        if result.returncode != 0:
            failed.append(pkg["id"])
            continue
        print(f"{pkg['id']}: cloned")

    if failed:
        print(f"could not clone: {', '.join(failed)}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
