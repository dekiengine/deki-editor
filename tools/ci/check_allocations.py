#!/usr/bin/env python3
"""Fail when package runtime code allocates without going through the engine.

Every allocation in code that runs on the device has to go through
Deki::Memory (usually via Deki::Buffer), and this script is what keeps that
true. It is not a style rule:

  * `new T[n]` cannot fail gracefully. ESP-IDF builds with exceptions off, so
    the bad_alloc reaches __cxa_allocate_exception and calls abort(), which
    reboots the board. `std::nothrow` does not help: libstdc++ implements it
    by calling the throwing form inside a try/catch that -fno-exceptions has
    removed, so it aborts identically.
  * `malloc` bypasses the placement decision. Deki::Memory is the only thing
    that knows whether this board has external RAM, whether a buffer is read
    every frame, and whether a peripheral has to reach it by DMA.
  * `heap_caps_*` hardcodes that decision into a package, which then behaves
    wrongly on the next board and does not compile off the ESP32 at all.

What is NOT checked, deliberately: `std::vector` and `std::string`. They call
the same aborting `operator new` internally, so a growth on a full heap ends
the same way. Fixing that means an allocator, not a lint, and it is tracked
separately.

Excluded directories, because they run on the host where exceptions are on
and the heap is a desktop's: `editor/` (a package's editor integration),
`tests/`, `generated/`, and any vendored `third_party/`.

A real exception gets a waiver on the same line or the line above, WITH a
reason:

    void* p = malloc(n);  // deki-alloc-ok: cJSON's allocator hook, freed by it

    python tools/ci/check_allocations.py             # check (CI)
    python tools/ci/check_allocations.py --verbose   # also list what was scanned
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_PACKAGES = REPO.parent / "modules"

SOURCE_SUFFIXES = {".cpp", ".cc", ".cxx", ".h", ".hpp", ".hh", ".inl"}

# A path is skipped when any of its directory names appears here. Matching on
# a segment rather than a prefix catches deki-2d/editor/ as well as a nested
# editor/widgets/.
SKIP_DIRS = {
    "editor",       # host-side: the package's editor integration
    "tests",        # host-side
    "generated",    # reflection codegen output
    "third_party",  # vendored; not ours to rewrite
    "build",
    ".git",
}

WAIVER = re.compile(r"//\s*deki-alloc-ok\s*:\s*(\S.*)")

# (name, pattern, what to do instead). The patterns run over source with
# comments and string literals blanked out, so a `new float[]` inside a
# comment explaining this rule does not trip it.
RULES: list[tuple[str, re.Pattern[str], str]] = [
    (
        "new[]",
        re.compile(r"\bnew\b(?![\w:])[^;{}()=]*?\["),
        "use Deki::Buffer<T> (it allocates through Deki::Memory and frees itself)",
    ),
    (
        "delete[]",
        re.compile(r"\bdelete\s*\[\s*\]"),
        "a Deki::Buffer frees itself; a raw delete[] here means the matching "
        "allocation is raw too, or worse, that it is not",
    ),
    (
        "malloc/calloc/realloc",
        re.compile(r"\b(?:std::)?(?:malloc|calloc|realloc)\s*\("),
        "use Deki::Memory::Allocate with a MemoryUse, or Deki::Buffer<T>",
    ),
    (
        "heap_caps_*",
        re.compile(r"\bheap_caps_[a-z_]+\s*\("),
        "let Deki::Memory choose the region: MemoryUse::Hot, Buffer or Dma. "
        "Only the ESP32 memory provider in the engine calls heap_caps directly",
    ),
]


def scrub(text: str) -> str:
    """Blank out comments and string literals, keeping every byte offset.

    Replacing rather than deleting means a match's offset still maps to the
    right line, so findings point at the real line number.
    """
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            for _ in range(2):
                if i < n:
                    out[i] = " "
                    i += 1
        elif c in "\"'":
            quote = c
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\":
                    out[i] = " "
                    i += 1
                if i < n:
                    if text[i] != "\n":
                        out[i] = " "
                    i += 1
            if i < n:
                out[i] = " "
                i += 1
        else:
            i += 1
    return "".join(out)


def waived(lines: list[str], lineno: int) -> str | None:
    """The waiver reason for a 1-based line, from it or the line above."""
    for idx in (lineno - 1, lineno - 2):
        if 0 <= idx < len(lines):
            m = WAIVER.search(lines[idx])
            if m:
                return m.group(1).strip()
    return None


def source_files(packages_dir: Path) -> list[Path]:
    files = []
    for path in sorted(packages_dir.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        rel = path.relative_to(packages_dir)
        if any(part in SKIP_DIRS for part in rel.parts[:-1]):
            continue
        files.append(path)
    return files


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--packages-dir", type=Path, default=DEFAULT_PACKAGES,
                    help=f"directory holding package checkouts (default {DEFAULT_PACKAGES})")
    ap.add_argument("--verbose", action="store_true",
                    help="list the files scanned and any waivers honoured")
    args = ap.parse_args(argv)

    if not args.packages_dir.is_dir():
        # Not a failure: a checkout without the packages next to it cannot
        # check them, and should not block on that.
        print(f"check_allocations: no packages at {args.packages_dir}; nothing to check")
        return 0

    files = source_files(args.packages_dir)
    findings: list[str] = []
    waivers: list[str] = []

    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        code = scrub(text)
        lines = text.splitlines()
        for name, pattern, advice in RULES:
            for m in pattern.finditer(code):
                lineno = code.count("\n", 0, m.start()) + 1
                rel = path.relative_to(args.packages_dir).as_posix()
                reason = waived(lines, lineno)
                if reason:
                    waivers.append(f"  {rel}:{lineno}  {name}  -- {reason}")
                    continue
                snippet = lines[lineno - 1].strip() if lineno - 1 < len(lines) else ""
                findings.append(f"{rel}:{lineno}: {name}\n    {snippet}\n    -> {advice}")

    if args.verbose:
        print(f"check_allocations: scanned {len(files)} file(s) under {args.packages_dir}")
        if waivers:
            print(f"check_allocations: {len(waivers)} waiver(s) honoured:")
            print("\n".join(waivers))

    if findings:
        print(f"check_allocations: {len(findings)} raw allocation(s) in package runtime code:\n")
        print("\n\n".join(findings))
        print(
            "\nOn the device these abort instead of returning null: ESP-IDF builds\n"
            "with exceptions off, so a failed new[] reboots the board. Allocate\n"
            "through Deki::Buffer<T> or Deki::Memory::Allocate, which return null\n"
            "and let the caller carry on without the thing it wanted.\n"
            "\nIf a case is genuinely unavoidable, waive it WITH a reason:\n"
            "    void* p = malloc(n);  // deki-alloc-ok: <why this one has to be raw>"
        )
        return 1

    print(f"check_allocations: {len(files)} file(s) clean"
          + (f", {len(waivers)} waived" if waivers else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
