#!/usr/bin/env python3
"""Compare `DekiEditor --render-scenes` output against a golden hash file.

    python tools/ci/check_golden.py render.txt tools/ci/golden-scaffold.txt
    python tools/ci/check_golden.py render.txt <golden> --record --reason "why"

`render.txt` is the verb's stdout: a `config:` line naming what produced the
pixels, then one "<hash>  <scene path>" line per scene.

WHY THE CONFIG MATTERS
An exact pixel hash is a property of one renderer, not of the project. Two
backends drawing the same scene correctly still differ: fill rules at pixel
edges, sub-pixel precision, filtering and blend rounding are all free to. So a
golden file holds one block per configuration, and a run is compared against
the block matching its own config. A second backend records its own block
instead of forcing this one's check to be loosened until both fit, which is
how a gate stops catching anything.

A config that is not in the file is a failure, not a pass: silently accepting
an unknown configuration is the same hole by another route.

Every golden scene must be rendered with the same hash, and every rendered
scene must be in the block, so an added or dropped scene fails too.

RECORDING A NEW GOLDEN
`--record --reason "..."` rewrites the block for the run's config and stamps
the reason into the file. Accepting a change is meant to be a deliberate act
with a note attached; a reference nobody approved looks exactly like a passing
test.

Exit 0 when they agree, 1 with one line per difference otherwise.
"""

from __future__ import annotations

import argparse
import datetime
import re
import sys
from pathlib import Path

LINE = re.compile(r"^\s*([0-9a-f]{16})\s+(\S.*?)\s*$")
CONFIG = re.compile(r"^\s*(?:#\s*)?config:\s*(.+?)\s*$")


def parse_render(path: Path) -> tuple[str, dict[str, str]]:
    """(config, {scene: hash}) from a --render-scenes stdout capture."""
    config = ""
    scenes: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line:
            continue
        m = CONFIG.match(line)
        if m:
            config = m.group(1)
            continue
        if line.startswith("#"):
            continue
        m = LINE.match(line)
        if m:
            scenes[m.group(2).replace("\\", "/")] = m.group(1)
    return config, scenes


def parse_golden(path: Path) -> dict[str, dict[str, str]]:
    """{config: {scene: hash}} from a golden file.

    Hash lines before any `config:` line belong to the empty config, which is
    what a file written before configs existed looks like.
    """
    blocks: dict[str, dict[str, str]] = {}
    current = ""
    blocks[current] = {}
    if not path.exists():
        return {}
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line:
            continue
        m = CONFIG.match(line)
        if m:
            current = m.group(1)
            blocks.setdefault(current, {})
            continue
        if line.startswith("#"):
            continue
        m = LINE.match(line)
        if m:
            blocks[current][m.group(2).replace("\\", "/")] = m.group(1)
    if not blocks.get(""):
        blocks.pop("", None)
    return blocks


def write_golden(path: Path, blocks: dict[str, dict[str, str]], header: list[str]) -> None:
    out: list[str] = list(header)
    for config in sorted(blocks):
        if not blocks[config]:
            continue
        out.append("")
        out.append(f"# config: {config}" if config else "# (no config recorded)")
        for scene in sorted(blocks[config]):
            out.append(f"{blocks[config][scene]}  {scene}")
    path.write_text("\n".join(out) + "\n", encoding="utf-8")


def read_header(path: Path) -> list[str]:
    """The leading comment block, minus any previous record stamp."""
    if not path.exists():
        return []
    header: list[str] = []
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line:
            break
        if not line.startswith("#"):
            break
        if line.startswith("# recorded:"):
            continue
        if CONFIG.match(line):
            break
        header.append(raw.rstrip())
    return header


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(add_help=True, description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("render")
    ap.add_argument("golden")
    ap.add_argument("--record", action="store_true",
                    help="rewrite this config's block from the render output")
    ap.add_argument("--reason", default="",
                    help="why the pixels changed; required with --record")
    args = ap.parse_args(argv)

    render_path = Path(args.render)
    golden_path = Path(args.golden)
    config, rendered = parse_render(render_path)

    if not rendered:
        print(f"check_golden: {args.render}: no '<hash>  <scene>' lines; "
              "did --render-scenes run?", file=sys.stderr)
        return 1
    if not config:
        print(f"check_golden: {args.render}: no 'config:' line; the golden is keyed by it. "
              "An older editor build does not print one.", file=sys.stderr)
        return 1

    blocks = parse_golden(golden_path)

    if args.record:
        if not args.reason.strip():
            print("check_golden: --record needs --reason: a reference nobody approved "
                  "looks exactly like a passing test.", file=sys.stderr)
            return 2
        header = read_header(golden_path)
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")
        header.append(f"# recorded: {stamp} - {args.reason.strip()}")
        blocks[config] = rendered
        write_golden(golden_path, blocks, header)
        print(f"check_golden: recorded {len(rendered)} scene(s) for '{config}'")
        return 0

    if config not in blocks:
        print(f"check_golden: no golden for this configuration:\n"
              f"    {config}\n"
              f"  the file has {len(blocks)}:", file=sys.stderr)
        for known in sorted(blocks):
            print(f"    {known or '(none)'}", file=sys.stderr)
        print("  Record one with --record --reason '...' if this configuration is "
              "meant to be gated.", file=sys.stderr)
        return 1

    golden = blocks[config]
    problems: list[str] = []
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
    if problems:
        print(f"check_golden: config was {config}", file=sys.stderr)
        return 1

    print(f"check_golden: {len(golden)} scene(s) match for '{config}'")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
