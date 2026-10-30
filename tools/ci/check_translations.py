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
against the sources under its editor/ folder and the names its components
show in the Inspector: component and property names, tooltips, categories,
descriptions, groups and enum values, read from its generated reflection
(generated/*Reflection*.gen.cpp, written by any build of the package). The
engine's own components are checked with the editor.
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
# Create-menu entries: the category and the name shown.
CREATE_MENU_RE = re.compile(
    r'(?:REGISTER_CREATE_MENU_ITEM\(\s*\w+\s*,|createMenu\.Register\()\s*"([^"]+)"\s*,\s*"([^"]+)"')
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
            for match in CREATE_MENU_RE.finditer(text):
                line = text.count("\n", 0, match.start()) + 1
                for key in match.groups():
                    found.setdefault(key, []).append(f"{path.relative_to(root.parent)}:{line}")
    return found


# ---- Names the Inspector shows, from generated reflection ----
#
# The order of a PropertyInfo initializer (deki/reflection/Property.h).
PROP_NAME, PROP_DISPLAY_NAME, PROP_TOOLTIP, PROP_ENUM_VALUES, PROP_GROUP = 0, 1, 4, 9, 21
# And of a ComponentMeta: DEKI_DISPLAY_NAME, and the Project Settings section's name.
META_DISPLAY_NAME, META_SETTINGS_SECTION = 16, 17


def strip_line_comments(text: str) -> str:
    """`text` without its // comments, leaving strings alone.

    The codegen writes a comment after the comma that ends a field, so without
    this the comment leads the next field and its literal is not read: every
    category and description went unchecked that way.
    """
    out, i, in_str = [], 0, False
    while i < len(text):
        c = text[i]
        if in_str:
            if c == "\\":
                out.append(text[i:i + 2])
                i += 2
                continue
            if c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif text.startswith("//", i):
            end = text.find("\n", i)
            i = len(text) if end < 0 else end
            continue
        out.append(c)
        i += 1
    return "".join(out)


def split_top_level(body: str) -> list[str]:
    """`body` split at commas outside strings, braces and parentheses."""
    parts, depth, start, i, in_str = [], 0, 0, 0, False
    while i < len(body):
        c = body[i]
        if in_str:
            if c == "\\":
                i += 1
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c in "({[":
            depth += 1
        elif c in ")}]":
            depth -= 1
        elif c == "," and depth == 0:
            parts.append(body[start:i].strip())
            start = i + 1
        i += 1
    tail = body[start:].strip()
    if tail:
        parts.append(tail)
    return parts


def braced_block(text: str, open_at: int) -> str:
    """The text inside the braces that open at `open_at`."""
    depth, i, in_str = 0, open_at, False
    while i < len(text):
        c = text[i]
        if in_str:
            if c == "\\":
                i += 1
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[open_at + 1:i]
        i += 1
    return ""


def literal(token: str) -> str | None:
    match = re.fullmatch(r'\s*"((?:[^"\\]|\\.)*)"\s*(?://.*)?', token, re.DOTALL)
    return decode(match.group(1)) if match else None


def class_display_name(name: str) -> str:
    """ComponentMeta::GetDisplayName: a space before a capital next to a lower-case letter."""
    out = ""
    for i, c in enumerate(name):
        if out and c.isupper():
            nxt = name[i + 1] if i + 1 < len(name) else ""
            if out[-1].islower() or nxt.islower():
                out += " "
        out += c
    return out


def nicify(name: str) -> str:
    """EditorNaming::NicifyName: "topColor" -> "Top Color", "scroll_speed" -> "Scroll Speed"."""
    out, cap = "", True
    for i, c in enumerate(name):
        if c in "_- ":
            if out and out[-1] != " ":
                out += " "
            cap = True
            continue
        prev = name[i - 1] if i else ""
        nxt = name[i + 1] if i + 1 < len(name) else ""
        if c.isupper() and (prev.islower() or (prev.isdigit() and nxt.islower())):
            out += " "
        elif c.isdigit() and prev.isalpha():
            out += " "
        out += c.upper() if cap else c
        cap = False
    return join_acronyms(out)


# EditorNaming.cpp's AcronymWords: words the camelCase split gets wrong.
ACRONYM_WORDS = {
    "Spi": "SPI", "Qspi": "QSPI", "I2c": "I2C", "I2s": "I2S", "Uart": "UART", "Gpio": "GPIO", "Mosi": "MOSI",
    "Miso": "MISO", "Sclk": "SCLK", "Sck": "SCK", "Sda": "SDA", "Scl": "SCL", "Cs": "CS", "Dc": "DC", "Rst": "RST",
    "Te": "TE", "Bl": "BL", "Tx": "TX", "Rx": "RX", "Rf": "RF", "Tcxo": "TCXO", "Dbm": "dBm", "Rgb": "RGB",
    "Rgba": "RGBA", "Psram": "PSRAM", "Gps": "GPS", "Ble": "BLE", "Imu": "IMU", "Rtc": "RTC", "Lcd": "LCD",
    "Http": "HTTP", "Url": "URL", "Usb": "USB", "Sd": "SD", "Fps": "FPS", "Ui": "UI", "Fsm": "FSM", "Ttf": "TTF",
    "Bdf": "BDF", "Pwm": "PWM",
}


def join_acronyms(split: str) -> str:
    """EditorNaming.cpp's JoinAcronyms: "I 2c Port" -> "I2C Port", "Dio 1 Pin" -> "DIO1 Pin"."""
    words, out, i = split.split(" "), [], 0
    while i < len(words):
        word = words[i]
        nxt = words[i + 1] if i + 1 < len(words) else ""
        if (word == "I" and nxt in ("2c", "2s")) or (word == "Lo" and nxt == "Ra") or (word in ("Ki", "Mi") and nxt == "B"):
            word = "I2" + nxt[1].upper() if word == "I" else word + nxt
            i += 1
        elif word in ("Dio", "Io", "Gpio") and nxt.isdigit():
            word = word.upper() + nxt
            i += 1
        else:
            word = ACRONYM_WORDS.get(word, word)
        out.append(word)
        i += 1
    return " ".join(out)


def reflection_texts(files: list[Path]) -> dict[str, list[str]]:
    found: dict[str, list[str]] = {}

    def add(key: str | None, where: str):
        if key and re.search(r"[A-Za-z]", key):
            found.setdefault(key, []).append(where)

    for path in files:
        text = strip_line_comments(path.read_text(encoding="utf-8", errors="replace"))
        enums ={m.group(1): [literal(v) for v in split_top_level(m.group(2))]
                 for m in re.finditer(r"static const char\* const (s_\w+_Values)\[\] = \{([^}]*)\}", text)}
        for m in re.finditer(r"ComponentMeta (\w+)_Meta = \{", text):
            fields = split_top_level(braced_block(text, m.end() - 1))
            where = f"{path.name}: {m.group(1)}"
            name = literal(fields[0]) if fields else None
            add(class_display_name(name) if name else None, where)
            add(literal(fields[2]) if len(fields) > 2 else None, where)  # category
            add(literal(fields[3]) if len(fields) > 3 else None, where)  # description
            add(literal(fields[META_DISPLAY_NAME]) if len(fields) > META_DISPLAY_NAME else None, where)
            add(literal(fields[META_SETTINGS_SECTION]) if len(fields) > META_SETTINGS_SECTION else None, where)
        for m in re.finditer(r"PropertyInfo (\w+)_Props\[\] = \{", text):
            block = braced_block(text, m.end() - 1)
            i = 0
            while True:
                start = block.find("{", i)
                if start < 0:
                    break
                entry = braced_block(block, start)
                i = start + len(entry) + 2
                fields = split_top_level(entry)
                if len(fields) <= PROP_GROUP:
                    continue
                where = f"{path.name}: {m.group(1)}"
                name, shown = literal(fields[PROP_NAME]), literal(fields[PROP_DISPLAY_NAME])
                add(shown or (nicify(name) if name else None), where)
                add(literal(fields[PROP_TOOLTIP]), where)
                add(literal(fields[PROP_GROUP]), where)
                for value in enums.get(fields[PROP_ENUM_VALUES].strip(), []):
                    add(value, where)
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
        reflection = sorted((args.package / "generated").glob("*Reflection*.gen.cpp"))
    else:
        sources = [EDITOR_ROOT / "src", EDITOR_ROOT / "deki-editor" / "src", EDITOR_ROOT / "deki-editor" / "include"]
        table_path = EDITOR_ROOT / "resources" / "lang" / f"{args.language}.json"
        reflection = sorted((EDITOR_ROOT / "deki-engine" / "include" / "deki" / "generated").glob("*Reflection*.gen.cpp"))

    table = load_tables(table_path)
    if table is None:
        return 1
    found = scan([s for s in sources if s.exists()])
    for key, where in reflection_texts(reflection).items():
        found.setdefault(key, []).extend(where)
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
