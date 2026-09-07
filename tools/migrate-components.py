#!/usr/bin/env python3
"""Migrate components from the DEKI_COMPONENT macro to macro-free declarations
(engine 0.11).

Before, a component declared itself inside the class and carried a GUID:

    class Walker : public Deki::Behaviour
    {
        DEKI_COMPONENT(Walker, Deki::Behaviour, "Demo", "6f3c...", "")
        DEKI_DESCRIPTION("Paces the object left and right.")
    public:
        ...
    };
    #include "generated/Walker.gen.h"

After, the class body declares nothing, the optional metadata sits above the
class, and the component's identity is its qualified C++ name:

    DEKI_CATEGORY("Demo")
    DEKI_DESCRIPTION("Paces the object left and right.")
    class Walker : public Deki::Behaviour
    {
    public:
        ...
    };

What the script does, per directory given:
  * headers: rewrites every DEKI_COMPONENT class as above; drops the generated
    include; moves DEKI_DESCRIPTION / DEKI_DISPLAY_NAME /
    DEKI_PROJECT_SETTINGS_SECTION (now DEKI_SETTINGS_SECTION) above the class.
  * sources and headers: `X::StaticType` -> `::Deki::TypeId<X>()`,
    `X::StaticGuid` -> `::Deki::TypeName<X>()`, and removes the
    DEKI_REGISTER_PROJECT_SETTINGS(_PLUGIN) lines (registration is generated).
  * records every GUID -> qualified name it saw into --map (JSON, merged).
  * --assets DIR...: rewrites JSON assets (scenes, boot scenes, graphs, a
    package.json) using the map: GUID strings become qualified names, numeric
    "typeId" values (FNV-1a of the GUID) become FNV-1a of the name, and a
    package's "componentGuids" feature lists become "componentTypes".

    python tools/migrate-components.py deki-engine/include modules/deki-2d --map guids.json
    python tools/migrate-components.py --map guids.json --assets deki-demo/assets deki-demo/platforms

Idempotent: a tree without DEKI_COMPONENT is left alone.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

SKIP_DIRS = {"generated", "build", ".git", "third_party", "_deps", "node_modules", "dist"}
CODE_EXT = {".h", ".hpp", ".cpp", ".cc", ".inl"}
ASSET_EXT = {".scene", ".json", ".anim", ".graph", ".fsm", ".prefab", ".tween", ".nodes", ".dproject"}

MACRO_RE = re.compile(
    r'DEKI_COMPONENT\(\s*([A-Za-z_]\w*)\s*,\s*([\w:]+)\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\)[ \t]*;?',
    re.DOTALL)
INNER_TAGS = {
    "DEKI_DESCRIPTION": "DEKI_DESCRIPTION",
    "DEKI_DISPLAY_NAME": "DEKI_DISPLAY_NAME",
    "DEKI_PROJECT_SETTINGS_SECTION": "DEKI_SETTINGS_SECTION",
}
INNER_TAG_RE = re.compile(r'(DEKI_DESCRIPTION|DEKI_DISPLAY_NAME|DEKI_PROJECT_SETTINGS_SECTION)\s*\(')
ACCESS_RE = re.compile(r'^\s*(public|protected|private)\s*:')


def fnv1a(s: str) -> int:
    h = 2166136261
    for ch in s.encode("utf-8"):
        h = ((h ^ ch) * 16777619) & 0xFFFFFFFF
    return h


def strip_code(text: str) -> str:
    """Blank out comments and string/char literals, keeping offsets."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i)); i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j])); i = j
        elif c in "\"'":
            q = c; j = i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(" " * (j - i)); i = j
        else:
            out.append(c); i += 1
    return "".join(out)


def enclosing_namespace(code: str, pos: int) -> str:
    """Namespaces open at `pos` (code with comments/strings blanked)."""
    stack: list[tuple[str, int]] = []  # (name, depth at its brace)
    depth = 0
    i = 0
    ns_re = re.compile(r'\bnamespace\s+([A-Za-z_][\w:]*)\s*\{')
    while i < pos:
        m = ns_re.match(code, i)
        if m:
            stack.append((m.group(1), depth))
            depth += 1
            i = m.end()
            continue
        c = code[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if stack and stack[-1][1] == depth:
                stack.pop()
        i += 1
    return "::".join(name for name, _ in stack)


def balanced_span(text: str, start: int) -> int:
    """End index (exclusive) of the parenthesised call starting at text[start]=='('."""
    depth = 0
    i = start
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


def line_bounds(text: str, pos: int) -> tuple[int, int]:
    a = text.rfind("\n", 0, pos) + 1
    b = text.find("\n", pos)
    b = len(text) if b < 0 else b + 1
    return a, b


def migrate_header(path: Path, text: str, guid_map: dict[str, str]) -> str | None:
    if "DEKI_COMPONENT(" not in text:
        return None
    original = text
    while True:
        m = MACRO_RE.search(text)
        if not m:
            break
        name, base, category, guid, _feature = m.groups()
        code = strip_code(text)
        # Ignore a match inside a comment (the macro's own documentation).
        if code[m.start():m.start() + 14] != "DEKI_COMPONENT":
            # blank it in a scratch copy is complex; instead skip by searching after it
            nxt = MACRO_RE.search(text, m.end())
            if not nxt:
                break
            # rotate: temporarily protect this comment occurrence
            text = text[:m.start()] + "DEKI_COMPONENT_DOC(" + text[m.start() + len("DEKI_COMPONENT("):]
            continue
        ns = enclosing_namespace(code, m.start())
        qualified = f"{ns}::{name}" if ns else name
        if guid:
            guid_map[guid] = qualified

        # ---- the class line above the macro
        cls_re = re.compile(r'^([ \t]*)(?:class|struct)\s+(?:[A-Z_][A-Z0-9_]*\s+)?' + re.escape(name) + r'\b[^\n]*$', re.M)
        cls_m = None
        for cm in cls_re.finditer(text, 0, m.start()):
            cls_m = cm
        if cls_m is None:
            print(f"  ! {path}: class {name} not found above its DEKI_COMPONENT; left as is", file=sys.stderr)
            text = text[:m.start()] + "DEKI_COMPONENT_UNMIGRATED(" + text[m.start() + len("DEKI_COMPONENT("):]
            continue
        indent = cls_m.group(1)

        # ---- remove the macro (whole lines) and collect the in-class markers after it
        mac_a, mac_b = line_bounds(text, m.start())
        _, mac_b = line_bounds(text, m.end() - 1)
        tags: list[str] = []
        if category:
            tags.append(f'DEKI_CATEGORY("{category}")')
        # scan following lines for in-class markers / blank lines
        body = text[mac_b:]
        cursor = 0
        while True:
            la = cursor
            lb = body.find("\n", la)
            lb = len(body) if lb < 0 else lb + 1
            line = body[la:lb]
            if line.strip() == "":
                cursor = lb
                if cursor >= len(body):
                    break
                continue
            tm = INNER_TAG_RE.match(line.lstrip())
            if tm:
                call_start = body.find("(", la)
                call_end = balanced_span(body, call_start)
                args = body[call_start + 1:call_end - 1].strip()
                tags.append(f"{INNER_TAGS[tm.group(1)]}({args})")
                # remove through end of that line
                _, le = line_bounds(body, call_end - 1)
                body = body[:la] + body[le:]
                continue
            break
        # The macro expanded to `public:`. Members after it stay public only if
        # the class was already in a public section or the next line opens one.
        rest_line = body[cursor:body.find("\n", cursor) if body.find("\n", cursor) >= 0 else len(body)]
        before = re.findall(r'\b(public|protected|private)\s*:', code[cls_m.end():m.start()])
        already_public = bool(before) and before[-1] == "public"
        needs_public = (not already_public and not ACCESS_RE.match(rest_line)
                        and not rest_line.strip().startswith("}"))
        mac_indent = re.match(r'[ \t]*', text[mac_a:mac_b]).group(0)
        replacement = (mac_indent + "public:\n") if needs_public else ""
        text = text[:mac_a] + replacement + body
        # collapse a blank line directly after the inserted public:
        if needs_public:
            text = re.sub(re.escape(mac_indent + "public:\n") + r'[ \t]*\n', mac_indent + "public:\n", text, count=1)

        # ---- insert the tags above the class line
        tag_block = "".join(f"{indent}{t}\n" for t in tags)
        text = text[:cls_m.start()] + tag_block + text[cls_m.start():]

        # ---- drop the generated include for this class
        text = re.sub(r'^[ \t]*#include\s+"(?:deki/)?generated/' + re.escape(name) + r'\.gen\.h"[^\n]*(?:\n|\Z)', "", text, flags=re.M)

    text = text.replace("DEKI_COMPONENT_DOC(", "DEKI_COMPONENT(").replace("DEKI_COMPONENT_UNMIGRATED(", "DEKI_COMPONENT(")
    return text if text != original else None


STATIC_TYPE_RE = re.compile(r'\b([A-Za-z_][\w:]*)::StaticType\b')
STATIC_GUID_RE = re.compile(r'\b([A-Za-z_][\w:]*)::StaticGuid\b')
REGISTER_RE = re.compile(r'^[ \t]*DEKI_REGISTER_PROJECT_SETTINGS(?:_PLUGIN)?\(\s*\w+\s*\)\s*;?[ \t]*\n', re.M)


def migrate_source(text: str) -> str | None:
    original = text

    def st(m: re.Match) -> str:
        n = m.group(1)
        if n.endswith("MissingComponent"):
            return m.group(0)
        return f"::Deki::TypeId<{n}>()"

    text = STATIC_TYPE_RE.sub(st, text)
    text = STATIC_GUID_RE.sub(lambda m: f"::Deki::TypeName<{m.group(1)}>()", text)
    text = REGISTER_RE.sub("", text)
    return text if text != original else None


def walk(root: Path, exts: set[str]):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            if Path(fn).suffix.lower() in exts:
                yield Path(dirpath) / fn


def migrate_code(roots: list[Path], guid_map: dict[str, str]) -> int:
    changed = 0
    for root in roots:
        files = [root] if root.is_file() else list(walk(root, CODE_EXT))
        for f in files:
            try:
                text = f.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            new = text
            if f.suffix.lower() in {".h", ".hpp", ".inl"}:
                h = migrate_header(f, new, guid_map)
                if h is not None:
                    new = h
            s = migrate_source(new)
            if s is not None:
                new = s
            if new != text:
                f.write_text(new, encoding="utf-8", newline="\n")
                changed += 1
                print(f"  migrated {f}")
    return changed


def migrate_assets(roots: list[Path], guid_map: dict[str, str]) -> int:
    if not guid_map:
        return 0
    by_hash = {fnv1a(g): fnv1a(n) for g, n in guid_map.items()}
    guid_re = re.compile('"(' + "|".join(re.escape(g) for g in guid_map) + ')"')
    typeid_re = re.compile(r'("typeId"\s*:\s*)(\d+)')
    changed = 0
    for root in roots:
        files = [root] if root.is_file() else list(walk(root, ASSET_EXT))
        for f in files:
            try:
                text = f.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            new = guid_re.sub(lambda m: '"' + guid_map[m.group(1)] + '"', text)
            new = typeid_re.sub(lambda m: m.group(1) + str(by_hash.get(int(m.group(2)), int(m.group(2)))), new)
            # package.json features list their components by type name now
            new = new.replace('"componentGuids"', '"componentTypes"')
            if new != text:
                f.write_text(new, encoding="utf-8", newline="\n")
                changed += 1
                print(f"  rewrote {f}")
    return changed


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dirs", nargs="*", type=Path, help="source trees (or files) to migrate")
    ap.add_argument("--map", type=Path, help="JSON file of GUID -> qualified name; read if present, written back merged")
    ap.add_argument("--assets", nargs="*", type=Path, default=[], help="asset trees whose JSON files get GUIDs rewritten")
    args = ap.parse_args()

    guid_map: dict[str, str] = {}
    if args.map and args.map.exists():
        guid_map = json.loads(args.map.read_text(encoding="utf-8"))

    n_code = migrate_code(args.dirs, guid_map) if args.dirs else 0
    n_assets = migrate_assets(args.assets, guid_map) if args.assets else 0

    if args.map:
        args.map.write_text(json.dumps(dict(sorted(guid_map.items())), indent=2) + "\n", encoding="utf-8")
    print(f"{n_code} source file(s) migrated, {n_assets} asset file(s) rewritten, {len(guid_map)} GUID(s) known")
    return 0


if __name__ == "__main__":
    sys.exit(main())
