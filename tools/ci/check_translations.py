#!/usr/bin/env python3
"""List the editor text that has no translation yet, and translations of text
that no longer exists.

The editor is written in English and shows other languages through a table
per language, resources/lang/<code>.json, which maps the English text to the
translation (see deki-editor/include/deki-editor/Localization.h). This script
finds the English text in the source the same way a reader would: string
literals passed to Tr, TrLabel and TrMark, and to the shared helpers that
translate what they are given (section headers, property rows, tooltips).

    python tools/ci/check_translations.py                 # report
    python tools/ci/check_translations.py --missing       # also list each missing text
    python tools/ci/check_translations.py --strict        # exit 1 when anything is missing
    python tools/ci/check_translations.py --language ja --package ../packages/deki-2d

A package keeps its own table in editor/lang/<code>.json and is checked
against the sources under its editor/ folder.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

EDITOR_ROOT = Path(__file__).resolve().parents[2]

# Calls whose first argument is English text that gets translated. The
# helpers translate inside, so their callers pass plain English.
TRANSLATING_CALLS = (
    "Tr",
    "TrLabel",
    "TrMark",
    "SchematicSectionBegin",
    "SchematicCollapsingHeader",
    "BeginPropertyRow",
    "PropertyRow",
    "SetNextRowTooltip",
    "Tooltip",
    "Note",
)

# A call name, "(", then one or more adjacent string literals.
CALL_RE = re.compile(
    r"(?<![A-Za-z0-9_])(?:" + "|".join(TRANSLATING_CALLS) + r")\s*\(\s*(?:ICON_\w+\s*)*((?:\"(?:[^\"\\\n]|\\.)*\"\s*)+)",
    re.DOTALL,
)
LITERAL_RE = re.compile(r"\"((?:[^\"\\\n]|\\.)*)\"")
ESCAPES = {"n": "\n", "t": "\t", '"': '"', "\\": "\\", "'": "'", "0": "\0"}


def decode(literal: str) -> str:
    """The text a C++ string literal holds. "\\xNN" is a byte, so a UTF-8
    character written as escapes ("\\xc2\\xb7") decodes to that character."""
    out, i = bytearray(), 0
    while i < len(literal):
        c = literal[i]
        if c == "\\" and i + 1 < len(literal):
            nxt = literal[i + 1]
            if nxt == "x":
                digits = re.match(r"[0-9A-Fa-f]{1,2}", literal[i + 2:])
                if digits:
                    out.append(int(digits.group(0), 16))
                    i += 2 + len(digits.group(0))
                    continue
            out += ESCAPES.get(nxt, nxt).encode("utf-8")
            i += 2
            continue
        out += c.encode("utf-8")
        i += 1
    return out.decode("utf-8", errors="replace")


def english_key(text: str) -> str | None:
    """The table key for a label: the text before any "##"; None when there is
    nothing to translate (an id only, a format with no words)."""
    # Icons (Private Use Area) and spaces in front are not part of the key, as in Tr().
    shown = re.sub(r"^[ \ue000-\uf8ff]+", "", text.split("##", 1)[0])
    words = re.sub(r"%[-+ 0#]*(\d+|\*)?(\.\d+)?(hh|h|ll|l|z|j|t)?[a-zA-Z%]", "", shown)
    return shown if re.search(r"[A-Za-z]", words) else None


def scan(sources: list[Path]) -> dict[str, list[str]]:
    found: dict[str, list[str]] = {}
    for root in sources:
        for path in sorted(root.rglob("*")):
            if path.suffix not in (".cpp", ".h", ".hpp") or not path.is_file():
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            # Comment lines (examples in a header) are not shown text. Blanked,
            # not removed, so line numbers stay right.
            text = re.sub(r"(?m)^[ \t]*//.*$", "", text)
            for match in CALL_RE.finditer(text):
                literal = "".join(decode(m.group(1)) for m in LITERAL_RE.finditer(match.group(1)))
                key = english_key(literal)
                if key is None:
                    continue
                line = text.count("\n", 0, match.start()) + 1
                found.setdefault(key, []).append(f"{path.relative_to(root.parent)}:{line}")
    return found


def load_tables(path: Path) -> dict[str, str] | None:
    """<code>.json and every file in <code>/, as the editor reads them."""
    files = ([path] if path.exists() else []) + sorted(path.with_suffix("").glob("*.json"))
    merged: dict[str, str] = {}
    for file in files:
        table = load_table(file)
        if table is None:
            return None
        for key, value in table.items():
            merged.setdefault(key, value)
    return merged


def load_table(path: Path) -> dict[str, str] | None:
    try:
        table = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        print(f"check_translations: {path}: not valid JSON: {e}")
        return None
    bad = [k for k, v in table.items() if not isinstance(v, str)]
    if not isinstance(table, dict) or bad:
        print(f"check_translations: {path}: every value must be a string ({len(bad)} are not)")
        return None
    return table


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--language", default="ja")
    parser.add_argument("--package", type=Path, help="check a package's editor/ folder instead of the editor")
    parser.add_argument("--missing", action="store_true", help="list each text without a translation")
    parser.add_argument("--strict", action="store_true", help="fail when any text has no translation")
    args = parser.parse_args()

    if args.package:
        sources = [args.package / "editor"]
        table_path = args.package / "editor" / "lang" / f"{args.language}.json"
    else:
        sources = [EDITOR_ROOT / "src", EDITOR_ROOT / "deki-editor" / "src", EDITOR_ROOT / "deki-editor" / "include"]
        table_path = EDITOR_ROOT / "resources" / "lang" / f"{args.language}.json"

    table = load_tables(table_path)
    if table is None:
        return 1
    found = scan([s for s in sources if s.exists()])
    missing = sorted(k for k in found if k not in table)
    stale = sorted(k for k in table if k not in found)

    print(f"check_translations: {args.language}: {len(found) - len(missing)} of {len(found)} texts translated, "
          f"{len(missing)} missing, {len(stale)} no longer in the source")
    if args.missing:
        for key in missing:
            print(f"  missing  {found[key][0]}: {key!r}")
    for key in stale:
        print(f"  unused   {key!r}")
    return 1 if args.strict and missing else 0


if __name__ == "__main__":
    sys.exit(main())
