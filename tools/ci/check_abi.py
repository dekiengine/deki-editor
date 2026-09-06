#!/usr/bin/env python3
"""Lock the virtual-function tables that package DLLs depend on.

Packages subclass a handful of engine and editor classes (Deki::Component,
Deki::Behaviour, Deki::IDisplay, DekiEditor::CustomEditor, ...). The order and
count of their virtual functions is part of the ABI: a virtual added in the
middle, removed, or reordered makes an already-built package call the wrong
slot, and nothing in the reflection handshake measures that (see the header
comment in deki-engine/include/deki/interop/ReflectionAbi.h, ABI v5). This script is
the measurement.

It extracts the `virtual` declarations of each listed class, in declaration
order, and compares them with tools/ci/abi.lock, which also records the
kDekiReflectionAbiVersion it was written at. The rule it enforces:

  * lock unchanged                              -> pass
  * virtuals changed AND the ABI version bumped -> pass; run `--update`
  * virtuals changed, ABI version the same      -> FAIL (bump it, same commit)
  * ABI version bumped, virtuals the same       -> pass (a sizeof-only bump)

    python tools/ci/check_abi.py            # compare (CI)
    python tools/ci/check_abi.py --update   # rewrite the lock after a bump
    python tools/ci/check_abi.py --show     # print what would be locked

Only `virtual`-keyworded declarations count: an `override` without `virtual`
reuses a base slot. Default-argument values are dropped (they are not part of
the vtable), `= 0` / `= default` too; parameter types, constness and the
return type are kept because a change there is a different function.
"""

from __future__ import annotations

import argparse
import difflib
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ENGINE = REPO / "deki-engine" / "include" / "deki"
EDITOR = REPO / "deki-editor" / "include" / "deki-editor"
ABI_HEADER = ENGINE / "interop" / "ReflectionAbi.h"
LOCK = REPO / "tools" / "ci" / "abi.lock"

# Class name -> header. The engine classes are the ones packages derive from
# or implement; the editor ones are the extension points a package's editor
# integration derives from. Adding a class here is itself a lock change.
CLASSES: dict[str, Path] = {
    "Deki::Component": ENGINE / "Component.h",
    "Deki::Behaviour": ENGINE / "Behaviour.h",
    "Deki::IDisplay": ENGINE / "providers" / "IDisplay.h",
    "Deki::IPackage": ENGINE / "providers" / "IPackage.h",
    "Deki::IRenderSystem": ENGINE / "providers" / "IRenderSystem.h",
    "Deki::IInputSystem": ENGINE / "providers" / "IInputSystem.h",
    "Deki::IFileSystem": ENGINE / "providers" / "IFileSystem.h",
    "Deki::IPower": ENGINE / "providers" / "IPower.h",
    "Deki::ITimeProvider": ENGINE / "providers" / "ITimeProvider.h",
    "Deki::IMemoryProvider": ENGINE / "providers" / "IMemoryProvider.h",
    "Deki::ICamera": ENGINE / "ICamera.h",
    "Deki::IClipProvider": ENGINE / "IClipProvider.h",
    "Deki::ISortableProvider": ENGINE / "ISortableProvider.h",
    "DekiEditor::CustomEditor": EDITOR / "CustomEditor.h",
    "DekiEditor::EditorExtension": EDITOR / "EditorExtension.h",
    "DekiEditor::EditorWindow": EDITOR / "EditorWindow.h",
}


class AbiError(Exception):
    pass


# --- header parsing ---------------------------------------------------------

def strip_comments(text: str) -> str:
    """Remove // and /* */ comments, keeping newlines so #if lines stay lines."""
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        two = text[i:i + 2]
        if two == "//":
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif two == "/*":
            j = text.find("*/", i + 2)
            if j < 0:
                break
            out.append("\n" * text.count("\n", i, j + 2))
            i = j + 2
        elif text[i] in "\"'":
            q = text[i]
            j = i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def class_body(text: str, unqualified: str) -> str:
    """The text between the braces of `class <macros> <unqualified> [: bases] {...}`."""
    pat = re.compile(r"\b(?:class|struct)\s+(?:[A-Z_][A-Z0-9_]*\s+)*" + re.escape(unqualified) + r"\b\s*(?:final\s*)?(?::[^{;]*)?\{")
    m = pat.search(text)
    if not m:
        raise AbiError(f"class {unqualified} not found")
    start = m.end()
    depth = 1
    i = start
    while i < len(text) and depth:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    if depth:
        raise AbiError(f"class {unqualified}: unbalanced braces")
    return text[start:i - 1]


def _drop_default_args(sig: str) -> str:
    """Remove `= expr` inside the parameter list (paren depth 1)."""
    out: list[str] = []
    depth = 0
    i = 0
    while i < len(sig):
        c = sig[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        if c == "=" and depth >= 1:
            # skip to the next ',' or ')' at this depth, stepping over string
            # and character literals whole
            d = 0
            j = i + 1
            while j < len(sig):
                ch = sig[j]
                if ch in "\"'":
                    k = j + 1
                    while k < len(sig) and sig[k] != ch:
                        k += 2 if sig[k] == "\\" else 1
                    j = k + 1
                    continue
                if ch in "([{<":
                    d += 1
                elif ch in ")]}>":
                    if d == 0:
                        break
                    d -= 1
                elif ch == "," and d == 0:
                    break
                j += 1
            i = j
            continue
        out.append(c)
        i += 1
    return "".join(out)


def extract_virtuals(body: str, where: str) -> list[str]:
    """Signatures of every `virtual` declaration at the class's own depth, in order."""
    if re.search(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b", body, re.M):
        raise AbiError(f"{where}: preprocessor conditional inside the class body; "
                       "the vtable would depend on build flags, which this lock cannot express")
    sigs: list[str] = []
    depth = 0
    i = 0
    n = len(body)
    while i < n:
        c = body[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        elif depth == 0 and body.startswith("virtual", i) and (i == 0 or not (body[i - 1].isalnum() or body[i - 1] == "_")) \
                and not (body[i + 7].isalnum() or body[i + 7] == "_"):
            # capture to the first ';' or '{' at paren depth 0
            j = i + 7
            pd = 0
            while j < n:
                ch = body[j]
                if ch == "(":
                    pd += 1
                elif ch == ")":
                    pd -= 1
                elif ch in ";{" and pd == 0:
                    break
                j += 1
            raw = body[i + 7:j]
            sig = " ".join(raw.split())
            sig = _drop_default_args(sig)
            sig = re.sub(r"\s*=\s*(0|default|delete)\s*$", "", sig)
            sig = re.sub(r"\s*\(\s*", "(", sig)
            sig = re.sub(r"\s*\)\s*", ")", sig).strip()
            sig = re.sub(r"\s*,\s*", ", ", sig)
            sig = re.sub(r"\)\s*const", ") const", sig)
            sigs.append(sig)
            i = j
            continue
        i += 1
    return sigs


def read_abi_version(header: Path = ABI_HEADER) -> int:
    m = re.search(r"kDekiReflectionAbiVersion\s*=\s*(\d+)", header.read_text(encoding="utf-8"))
    if not m:
        raise AbiError(f"{header}: kDekiReflectionAbiVersion not found")
    return int(m.group(1))


def collect(classes: dict[str, Path] = CLASSES) -> dict[str, list[str]]:
    result: dict[str, list[str]] = {}
    for name, header in classes.items():
        if not header.exists():
            raise AbiError(f"{name}: header {header} does not exist")
        text = strip_comments(header.read_text(encoding="utf-8"))
        unqualified = name.rsplit("::", 1)[-1]
        try:
            body = class_body(text, unqualified)
        except AbiError as e:
            raise AbiError(f"{header}: {e}") from None
        result[name] = extract_virtuals(body, f"{name} ({header.name})")
    return result


# --- lock file --------------------------------------------------------------

def render_lock(version: int, virtuals: dict[str, list[str]]) -> str:
    lines = [
        "# Virtual-function tables package DLLs depend on. Generated by",
        "# tools/ci/check_abi.py --update; do not edit by hand.",
        "#",
        "# A change below is an ABI break: bump kDekiReflectionAbiVersion in",
        "# deki-engine/include/deki/interop/ReflectionAbi.h in the same commit and",
        "# say what changed in deki-engine/CHANGELOG.md.",
        f"abi_version: {version}",
    ]
    for name, sigs in virtuals.items():
        lines.append("")
        lines.append(f"class {name} ({len(sigs)} virtual)")
        lines.extend(f"  {s}" for s in sigs)
    return "\n".join(lines) + "\n"


def parse_lock(text: str) -> tuple[int, dict[str, list[str]]]:
    version = -1
    virtuals: dict[str, list[str]] = {}
    current: list[str] | None = None
    for raw in text.splitlines():
        line = raw.rstrip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("abi_version:"):
            version = int(line.split(":", 1)[1])
        elif line.startswith("class "):
            name = line[6:].split(" (", 1)[0].strip()
            current = virtuals.setdefault(name, [])
        elif line.startswith("  ") and current is not None:
            current.append(line.strip())
    if version < 0:
        raise AbiError(f"{LOCK}: no abi_version line")
    return version, virtuals


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--update", action="store_true", help="rewrite the lock from the current headers")
    ap.add_argument("--show", action="store_true", help="print the current tables and exit")
    args = ap.parse_args(argv)

    try:
        version = read_abi_version()
        current = collect()
    except AbiError as e:
        print(f"check_abi: {e}", file=sys.stderr)
        return 1

    rendered = render_lock(version, current)
    if args.show:
        sys.stdout.write(rendered)
        return 0
    if args.update:
        LOCK.write_text(rendered, encoding="utf-8", newline="\n")
        print(f"check_abi: wrote {LOCK.relative_to(REPO)} at ABI v{version}")
        return 0

    if not LOCK.exists():
        print(f"check_abi: {LOCK.relative_to(REPO)} is missing; run --update", file=sys.stderr)
        return 1
    try:
        locked_version, locked = parse_lock(LOCK.read_text(encoding="utf-8"))
    except (AbiError, ValueError) as e:
        print(f"check_abi: {e}", file=sys.stderr)
        return 1

    same_tables = locked == current
    if same_tables and locked_version == version:
        total = sum(len(v) for v in current.values())
        print(f"check_abi: {len(current)} classes, {total} virtuals, ABI v{version}: unchanged")
        return 0
    if same_tables:
        print(f"check_abi: ABI version bumped {locked_version} -> {version} with the same tables "
              "(a layout-only bump); run --update to record it")
        return 0

    diff = "\n".join(difflib.unified_diff(
        render_lock(locked_version, locked).splitlines(),
        rendered.splitlines(), fromfile="abi.lock", tofile="headers", lineterm="", n=2))
    if version != locked_version:
        print(f"check_abi: tables changed and ABI version bumped {locked_version} -> {version}; "
              "run --update to record the new lock:\n" + diff)
        return 0
    print("check_abi: the virtual tables changed but kDekiReflectionAbiVersion is still "
          f"{version}. Packages built against the old layout would call the wrong slot. "
          "Bump the constant in deki-engine/include/deki/interop/ReflectionAbi.h, note it in "
          "deki-engine/CHANGELOG.md, then run --update.\n" + diff, file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
