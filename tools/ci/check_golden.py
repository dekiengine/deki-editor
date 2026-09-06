#!/usr/bin/env python3
"""Compare `DekiEditor --render-scenes` output against a golden hash file.

    python tools/ci/check_golden.py render.txt tools/ci/golden-scaffold.txt

`render.txt` is the verb's stdout: one "<hash>  <scene path>" line per scene
after the header. The golden file holds the same lines (comments with `#` and
blank lines ignored). Every golden scene must be rendered with the same hash,
and every rendered scene must be in the golden file, so an added or dropped
scene is a failure too, not just a changed pixel.

Exit 0 when they agree, 1 with one line per difference otherwise. To record a
new golden after an intended change: copy the hash lines from render.txt and
say why in the commit.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

LINE = re.compile(r"^\s*([0-9a-f]{16})\s+(\S.*?)\s*$")


def parse(path: Path) -> dict[str, str]:
    scenes: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = LINE.match(line)
        if m:
            scenes[m.group(2).replace("\\", "/")] = m.group(1)
    return scenes


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    rendered = parse(Path(argv[0]))
    golden = parse(Path(argv[1]))
    problems: list[str] = []
    if not rendered:
        problems.append(f"{argv[0]}: no '<hash>  <scene>' lines; did --render-scenes run?")
    for scene, hash_ in golden.items():
        got = rendered.get(scene)
        if got is None:
            problems.append(f"{scene}: in the golden file but not rendered")
        elif got != hash_:
            problems.append(f"{scene}: rendered {got}, golden {hash_}")
    for scene in rendered:
        if scene not in golden:
            problems.append(f"{scene}: rendered but not in the golden file")
    for p in problems:
        print(f"check_golden: {p}", file=sys.stderr)
    if not problems:
        print(f"check_golden: {len(golden)} scene(s) match")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
