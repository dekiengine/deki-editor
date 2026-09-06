#!/usr/bin/env python3
"""One-off for engine 0.10: wrap every engine source/header in `namespace Deki`.

    wrap-engine-namespace.py <deki-engine/src> [--dry-run]

Skips generated/, third_party/, interop/ (the C ABI surface stays global) and
files that already open `namespace Deki`. The opening brace goes after the
file's preamble (comments, preprocessor lines, single-line extern "C"
declarations, `using namespace`); the closing brace goes at the end, after any
trailing `#include "generated/...gen.h"` (those stubs are namespace-neutral).
"""

import os
import re
import sys

SKIP_DIRS = {"generated", "third_party", "interop", "build"}
EXTS = {".h", ".hpp", ".cpp", ".inl"}
PREAMBLE = re.compile(r'^\s*($|//|/\*|\*|#|extern\s+"C"\s+[^{]*;\s*(//.*)?$|using\s+namespace\s+[A-Za-z_:]+\s*;)')


def wrap(path, dry_run):
    raw = open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
    nl = "\r\n" if "\r\n" in raw else "\n"
    lines = raw.split(nl)
    if any(re.match(r"^\s*namespace\s+Deki\b", l) for l in lines):
        return "already"
    # preamble: skip block comments as units
    i = 0
    in_block = False
    while i < len(lines):
        l = lines[i]
        if in_block:
            if "*/" in l:
                in_block = False
            i += 1
            continue
        if l.lstrip().startswith("/*") and "*/" not in l:
            in_block = True
            i += 1
            continue
        if PREAMBLE.match(l):
            i += 1
            continue
        break
    if i >= len(lines):
        return "empty"  # nothing but preamble (macro-only header): leave alone
    # back up over trailing blank lines so the brace sits right before the code
    while i > 0 and lines[i - 1].strip() == "":
        i -= 1
    body = lines[:i] + ["", "namespace Deki", "{", ""] + lines[i:]
    # closing brace before a final run of blank lines
    while body and body[-1].strip() == "":
        body.pop()
    body += ["", "}  // namespace Deki", ""]
    new = nl.join(body)
    if not dry_run:
        open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(new)
    return "wrapped"


def main(argv):
    dry = "--dry-run" in argv
    roots = [a for a in argv if not a.startswith("--")]
    counts = {}
    for root in roots:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for f in sorted(filenames):
                if os.path.splitext(f)[1] not in EXTS:
                    continue
                p = os.path.join(dirpath, f)
                r = wrap(p, dry)
                counts[r] = counts.get(r, 0) + 1
                if r != "wrapped":
                    print(f"{r}: {p}")
    print(counts)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
