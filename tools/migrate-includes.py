#!/usr/bin/env python3
"""Rewrite `#include` lines for the engine's public header layout (engine 0.10).

The engine's public headers moved from `deki-engine/src/` to
`deki-engine/include/deki/`, and dropped the redundant `Deki` file-name prefix
that the 0.10 namespace release already dropped from the types:

    #include "DekiObject.h"               ->  #include <deki/Object.h>
    #include "reflection/DekiProperty.h"  ->  #include <deki/reflection/Property.h>
    #include "providers/IDekiDisplay.h"   ->  #include <deki/providers/IDisplay.h>
    #include "Scene.h"                    ->  #include <deki/Scene.h>

Everything still under `deki-engine/src/` is internal to the engine build and
not reachable from a package; a consumer that included one of those headers
gets a warning from this tool and a compile error afterwards, which is the
point.

    migrate-includes.py <dir-or-file>...          consumer code (packages, projects, the editor)
    migrate-includes.py --engine <engine-root>    the engine's own tree (resolves relative includes)
    migrate-includes.py --dry-run ...             report what would change
    migrate-includes.py --emit-map <engine-src>   print the old -> new table for a pre-move src/ tree

The table of moves lives beside this script in `migrate-includes-map.txt`
(one `old-spelling new-spelling` pair per line), generated once from the
pre-move tree with `--emit-map` and committed, so every repository is
rewritten from the same list. Only an exact old spelling is rewritten: a
package's own `RenderSystem.h` is not the engine's `providers/IDekiRenderSystem.h`.
Idempotent: a line already in the new form is left alone.
"""

from __future__ import annotations

import argparse
import os
import posixpath
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MAP_FILE = os.path.join(HERE, "migrate-includes-map.txt")

EXTS = {".h", ".hpp", ".hh", ".inl", ".c", ".cc", ".cpp", ".cxx", ".ipp", ".txt", ".cmake", ".py", ".md"}
SKIP_DIRS = {".git", "build", "generated", "third_party", "_deps", "node_modules", "dist", "bin", "packages"}

# Headers that stay in src/: engine-internal, never part of a package's API.
PRIVATE = {
    "AuthoredFormat.h",
    "InputKeys.h",
    "LifecycleManager.h",
    "MissingComponent.h",
    "providers/DekiProviderRegistry.h",
    "providers/DekiProviderRegistryBase.h",
    "platforms/editor/EditorDisplay.h",
}

INCLUDE_RE = re.compile(r'^(\s*#\s*include\s*)([<"])([^>"]+)([>"])(.*)$')


def new_name(basename: str) -> str:
    """DekiObject.h -> Object.h, IDekiDisplay.h -> IDisplay.h, Scene.h -> Scene.h."""
    m = re.fullmatch(r"(I?)Deki([A-Z].*)", basename)
    return m.group(1) + m.group(2) if m else basename


def plan_moves(engine_src: str) -> list[tuple[str, str]]:
    """(old spelling relative to src, new spelling relative to include) for every public header."""
    moves = []
    for root, dirs, files in os.walk(engine_src):
        dirs[:] = sorted(d for d in dirs if d != "generated")
        rel_dir = os.path.relpath(root, engine_src).replace("\\", "/")
        rel_dir = "" if rel_dir == "." else rel_dir + "/"
        for f in sorted(files):
            if not f.endswith(".h"):
                continue
            old = rel_dir + f
            if old in PRIVATE:
                continue
            moves.append((old, "deki/" + rel_dir + new_name(f)))
    return moves


def load_map(path: str = MAP_FILE) -> dict[str, str]:
    table: dict[str, str] = {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            old, new = line.split()
            table[old] = new
    return table


def _norm(p: str) -> str:
    p = posixpath.normpath(p.replace("\\", "/"))
    return "" if p == "." else p


def rewrite_text(text: str, table: dict[str, str], *, engine_dir: str | None = None,
                 warn=None) -> tuple[str, int]:
    """Rewrite include lines. `engine_dir` is the including file's directory
    relative to the engine's old src/ root (engine mode); None for consumers."""
    out_lines = []
    changed = 0
    for line in text.splitlines(keepends=True):
        m = INCLUDE_RE.match(line.rstrip("\r\n"))
        if not m:
            out_lines.append(line)
            continue
        prefix, open_q, spelling, close_q, rest = m.groups()
        eol = line[len(line.rstrip("\r\n")):]
        target = None

        if engine_dir is not None:
            # A quote include resolves against the including file's directory
            # first ("PropertyRef.h" inside reflection/, "../DekiObject.h").
            if open_q == '"':
                local = _norm(posixpath.join(engine_dir, spelling))
                if local in table:
                    target = table[local]
                elif local.startswith("generated/"):
                    target = "deki/" + local
            if target is None and spelling in table:
                target = table[spelling]
            if target is None and spelling.startswith("generated/"):
                target = "deki/" + spelling
        elif spelling in table:
            target = table[spelling]

        if target is None:
            if warn and engine_dir is None and spelling in PRIVATE:
                warn(f"includes engine-internal header {spelling!r}; it is no longer reachable")
            out_lines.append(line)
            continue

        # Engine generated headers are quote-included by path from the include root
        # (the codegen's stub scan keys on that form); everything else angle-bracketed.
        if target.startswith("deki/generated/"):
            new_line = f'{prefix}"{target}"{rest}{eol}'
        else:
            new_line = f"{prefix}<{target}>{rest}{eol}"
        if new_line != line:
            changed += 1
        out_lines.append(new_line)
    return "".join(out_lines), changed


def engine_relative_dir(path: str, engine_root: str) -> str | None:
    """Directory of an engine file relative to the OLD src/ root: include/deki/X
    and src/X both map to X (the move kept directory structure)."""
    rel = os.path.relpath(path, engine_root).replace("\\", "/")
    for prefix in ("include/deki/", "src/"):
        if rel.startswith(prefix):
            return _norm(posixpath.dirname(rel[len(prefix):]))
    return None


def iter_files(paths: list[str]):
    for p in paths:
        if os.path.isfile(p):
            yield p
            continue
        for root, dirs, files in os.walk(p):
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
            for f in files:
                if os.path.splitext(f)[1] in EXTS:
                    yield os.path.join(root, f)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--engine", metavar="ENGINE_ROOT", help="rewrite the engine tree itself (resolves relative includes)")
    ap.add_argument("--emit-map", metavar="ENGINE_SRC", help="print the move table for a pre-move src/ tree and exit")
    ap.add_argument("--map", default=MAP_FILE, help="move table to use (default: migrate-includes-map.txt beside this script)")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)

    if args.emit_map:
        print("# old spelling (relative to deki-engine/src)  new spelling (relative to deki-engine/include)")
        print("# generated by tools/migrate-includes.py --emit-map; consumed by the same tool")
        for old, new in plan_moves(args.emit_map):
            print(f"{old} {new}")
        return 0

    table = load_map(args.map)
    if args.engine:
        roots = [os.path.join(args.engine, "include"), os.path.join(args.engine, "src"), os.path.join(args.engine, "entry"),
                 os.path.join(args.engine, "tools"), os.path.join(args.engine, "cmake")]
        files = [f for r in roots if os.path.isdir(r) for f in iter_files([r])]
    else:
        if not args.paths:
            ap.error("give paths to rewrite, or --engine / --emit-map")
        files = list(iter_files(args.paths))

    total_files = total_lines = 0
    for path in files:
        try:
            with open(path, encoding="utf-8", newline="") as fh:
                text = fh.read()
        except (UnicodeDecodeError, OSError):
            continue
        engine_dir = engine_relative_dir(path, args.engine) if args.engine else None
        if args.engine and engine_dir is None:
            engine_dir = ""  # tools/, cmake/: consumer-style spellings, no relative resolution
        warnings: list[str] = []
        new_text, changed = rewrite_text(text, table, engine_dir=engine_dir, warn=warnings.append)
        for w in warnings:
            print(f"warning: {path}: {w}")
        if not changed:
            continue
        total_files += 1
        total_lines += changed
        print(f"{'would rewrite' if args.dry_run else 'rewrote'} {changed:3d} include(s): {path}")
        if not args.dry_run:
            with open(path, "w", encoding="utf-8", newline="") as fh:
                fh.write(new_text)
    print(f"{total_lines} include line(s) in {total_files} file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
