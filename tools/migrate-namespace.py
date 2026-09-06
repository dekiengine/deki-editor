#!/usr/bin/env python3
"""Rename Deki engine types for engine 0.10: everything in `namespace Deki`, no prefix.

    migrate-namespace.py <dir-or-file>...            consumer code (packages, projects, editor)
    migrate-namespace.py --engine <dir>              the engine's own sources (inside namespace Deki)
    migrate-namespace.py --dry-run ...               report what would change

Consumer code: `DekiObject` -> `Deki::Object`, `Scene` -> `Deki::Scene`,
`SceneFormat::X` -> `Deki::SceneFormat::X`, `::LogLevel` -> `::Deki::LogLevel`;
global forward declarations `class Scene;` -> `namespace Deki { class Scene; }`.
`#include` lines and string literals are left alone (file names did not change;
a literal naming a type is a display string). Macro bodies (`#define` lines and
their continuations) get fully qualified `::Deki::` names because they expand
wherever the user writes them.

Engine sources (--engine): the same map, but the new names are unqualified
because the file body is inside `namespace Deki`; macro bodies still get
`::Deki::`. Files that are NOT wrapped (the C interop surface) are treated as
consumer code.

Word-boundary matching only; a name preceded by `.` or `->` is a member and is
skipped, one preceded by `Deki::` is already migrated.
"""

import argparse
import os
import re
import sys

# Prefixed engine globals -> new unprefixed name inside namespace Deki.
MOVED = {
    "DekiObject": "Object",
    "DekiComponent": "Component",
    "DekiBehaviour": "Behaviour",
    "DekiEngine": "Engine",
    "DekiEngineSettings": "EngineSettings",
    "DekiEngineHost": "EngineHost",
    "DekiMain": "Main",
    "DekiColorFormat": "ColorFormat",
    "DekiFPSCounter": "FPSCounter",
    "DekiLogSystem": "LogSystem",
    "DekiWorldTransform": "WorldTransform",
    "DekiTime": "Time",
    "DekiVector2": "Vector2",
    "DekiVector3": "Vector3",
    "DekiRect": "Rect",
    "DekiFileSystem": "FileSystem",
    "DekiMemory": "Memory",
    "DekiMemoryStats": "MemoryStats",
    "DekiMemorySubsystem": "MemorySubsystem",
    "DekiPower": "Power",
    "DekiProviderRegistry": "ProviderRegistry",
    "DekiProviderRegistryBase": "ProviderRegistryBase",
    "IDekiDisplay": "IDisplay",
    "IDekiFileSystem": "IFileSystem",
    "IDekiInputSystem": "IInputSystem",
    "IDekiPackage": "IPackage",
    "IDekiPower": "IPower",
    "IDekiRenderSystem": "IRenderSystem",
    "DekiFieldRef": "FieldRef",
    "DekiPropertyInfo": "PropertyInfo",
    "DekiComponentMeta": "ComponentMeta",
    "DekiPropertyType": "PropertyType",
    "DekiPhysicalUnit": "PhysicalUnit",
    "DekiTransformField": "TransformField",
    "DekiTransformFields": "TransformFields",
    "DekiTransformFieldNames": "TransformFieldNames",
    "DekiSettingsTypeInfo": "SettingsTypeInfo",
    "DekiSettingsRegistry": "SettingsRegistry",
    "DekiHashString": "HashString",
    "DekiHashStringLen": "HashStringLen",
}

# Engine globals that keep their name but move into namespace Deki.
QUALIFY = [
    "Scene", "SceneSystem", "LifecycleManager", "MissingComponent", "SetupComponent",
    "EditorAutoSetupRegistrar", "PlatformSetupComponent", "PackageConfig", "PackageState",
    "ICamera", "ProjectionMode", "UnitMode", "IClipProvider", "ISortableProvider",
    "IMemoryProvider", "ITimeProvider", "Mat4", "ComponentRegistry", "ComponentFactory",
    "PropertyRef", "PropertyBinding", "ObjectRef", "ObjectRefBase", "NodeRef",
    "CoreProjectSettings", "DisplayProjectSettings", "UnitProjectSettings",
    "DesktopFileSystem", "DesktopMemoryProvider", "EditorDisplay",
    "ESP32FileSystem", "ESP32MemoryProvider", "ESP32TimeProvider",
    "ComponentType", "LogLevel",
    # Engine callback aliases (UpdateCallback, LogCallback, AssetResolver, ...) are
    # deliberately NOT listed: the names are generic and collide with aliases that
    # packages define for themselves (deki-tween has its own UpdateCallback).
    # Qualify a real use of the engine's by hand.
    # free functions (reflection/DekiProperty.h)
    "GetPropertyValue", "SetPropertyValue", "GetPropertyRef", "kTransformRefComponent", "kVariableRefComponent",
]

# Engine namespaces that become nested in Deki (old -> new name).
NAMESPACES = {
    "SceneFormat": "SceneFormat",
    "ProjectSettings": "ProjectSettings",
    "ComponentInterfaceAdapters": "ComponentInterfaceAdapters",
    "InputKeys": "InputKeys",
    "deki_refl": "deki_refl",
    "DekiStorage": "Storage",
    "DekiMath": "Math",
}

# ComponentFactory leaves SceneFormat for Deki proper.
SPECIAL = {"SceneFormat::ComponentFactory": "ComponentFactory"}
KEEP = ("SceneFormat::NodeFactory",)  # deki-nodegraph's own SceneFormat namespace, not the engine's

SKIP_DIRS = {"third_party", "generated", "build", "build-alt", ".git", "_deps", "node_modules"}
EXTS = {".h", ".hpp", ".hh", ".cpp", ".cc", ".cxx", ".inl", ".ipp"}

WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*")


def qualify(new_name, inside_engine, in_macro):
    if in_macro:
        return "::Deki::" + new_name
    if inside_engine:
        return new_name
    return "Deki::" + new_name


def rewrite_token(tok, before, inside_engine, in_macro):
    """Return the replacement for identifier token `tok` (may contain ::), or None."""
    # already qualified / member access / part of another qualification
    if before.endswith("Deki::") or before.endswith(".") or before.endswith("->"):
        return None
    # `::X` written explicitly is fine: the replacement inserts `Deki::` after it.
    for keep in KEEP:
        if tok == keep or tok.startswith(keep + "::"):
            return None
    for special, target in SPECIAL.items():
        if tok == special or tok.startswith(special + "::"):
            return qualify(target + tok[len(special):], inside_engine, in_macro)

    parts = tok.split("::")
    head = parts[0]
    rest = "::".join(parts[1:])

    if head in NAMESPACES:
        new_ns = NAMESPACES[head]
        if len(parts) == 1:
            # bare namespace name: `namespace DekiMath`, `using namespace DekiStorage;`
            if before.rstrip().endswith("namespace"):
                return new_ns if new_ns != head else None
            if inside_engine and not in_macro:
                return new_ns if new_ns != head else None
            return qualify(new_ns, inside_engine, in_macro)
        if inside_engine and not in_macro:
            return (new_ns + "::" + rest) if new_ns != head else None
        return qualify(new_ns + "::" + rest, inside_engine, in_macro)

    # every segment can be a moved type: `DekiEngine::DekiEngine()` (constructor
    # definition), `DekiComponent::~DekiComponent` splits at `~` so is covered too
    mapped = [MOVED.get(seg, seg) for seg in parts]
    if head in MOVED or head in QUALIFY:
        new = "::".join(mapped)
        if inside_engine and not in_macro:
            return new if new != tok else None
        return qualify(new, inside_engine, in_macro)
    if mapped != parts:
        # `Engine::DekiEngine` (a later segment only): rename in place, no qualification
        return "::".join(mapped)
    return None


DECL_WORDS = {"new", "return", "const", "throw", "case", "typename", "class", "struct", "enum", "friend",
              "public", "private", "protected", "using", "else", "delete", "sizeof", "typedef"}


def looks_like_function_name(before, rest):
    """`void SetupComponent(` / `bool Scene(`: a function named like an engine type."""
    if not rest.startswith("("):
        return False
    m = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*[*&]*\s*$", before)
    return bool(m) and m.group(1) not in DECL_WORDS and not before.rstrip().endswith(("::", ",", "(", "<", "="))


def macro_params(line):
    """Parameter names of a `#define NAME(a, b)` line; they are never types."""
    m = re.match(r"\s*#\s*define\s+[A-Za-z_][A-Za-z0-9_]*\(([^)]*)\)", line)
    return {x.strip() for x in m.group(1).split(",")} if m else set()


COMMENTS = False  # --comments: rename inside comments as well (never inside string literals)


def process_comment(text, inside_engine, stats):
    """Rename type names mentioned in comment prose; `DekiObject.h` style file names stay."""
    def sub(m):
        tok = m.group(0)
        after = text[m.end():m.end() + 4]
        if after.startswith(".h") or after.startswith(".cpp") or after.startswith(".inl"):
            return tok
        before = text[:m.start()]
        rep_ = rewrite_token(tok, before, inside_engine, False)
        if rep_ is None:
            return tok
        stats[tok] = stats.get(tok, 0) + 1
        return rep_
    return WORD.sub(sub, text)


def process_code(line, inside_engine, in_macro, stats, skip_names=()):
    """Rewrite one line of code, skipping string and char literals."""
    out = []
    i = 0
    n = len(line)
    while i < n:
        c = line[i]
        if c == '"' or c == "'":
            # copy the literal verbatim
            j = i + 1
            while j < n:
                if line[j] == "\\":
                    j += 2
                    continue
                if line[j] == c:
                    j += 1
                    break
                j += 1
            out.append(line[i:j])
            i = j
            continue
        if line.startswith("//", i):
            out.append(process_comment(line[i:], inside_engine, stats) if COMMENTS else line[i:])
            break
        m = WORD.match(line, i)
        if m and (i == 0 or not (line[i - 1].isalnum() or line[i - 1] == "_")):
            tok = m.group(0)
            before = "".join(out)
            rest = line[m.end():].lstrip()
            # An enumerator or a bare declarator that happens to share a name with an
            # engine type (`enum class AssetCategory { ..., Scene, ... }`): a QUALIFY
            # name alone on its line followed by `,`, `=` or nothing is not a type use.
            enumerator = (before.strip() == "" and tok in QUALIFY and (rest == "" or rest[0] in ",="))
            skip = enumerator or tok in skip_names or (tok in QUALIFY and looks_like_function_name(before, rest))
            rep = None if skip else rewrite_token(tok, before, inside_engine, in_macro)
            if rep is not None:
                if before.endswith("::") and rep.startswith("::Deki::"):
                    rep = rep[2:]  # `::X` in a macro body: avoid `::::Deki::X`
                stats[tok] = stats.get(tok, 0) + 1
                out.append(rep)
            else:
                out.append(tok)
            i = m.end()
            continue
        out.append(c)
        i += 1
    return "".join(out)


FWD = re.compile(r"^(\s*)(class|struct|enum class|enum)\s+([A-Za-z_][A-Za-z0-9_]*)\s*;\s*(//.*)?$")


def process_file(path, inside_engine, dry_run):
    raw = open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
    nl = "\r\n" if "\r\n" in raw else "\n"
    lines = raw.split(nl)
    stats = {}
    out_lines = []
    in_macro = False
    in_block_comment = False
    params = set()
    for line in lines:
        stripped = line.lstrip()
        if in_block_comment:
            out_lines.append(process_comment(line, inside_engine, stats) if COMMENTS else line)
            if "*/" in line:
                in_block_comment = False
            continue
        if stripped.startswith("/*") and "*/" not in stripped:
            in_block_comment = True
            out_lines.append(process_comment(line, inside_engine, stats) if COMMENTS else line)
            continue
        if stripped.startswith("#include") or stripped.startswith("#pragma"):
            out_lines.append(line)
            in_macro = False
            continue
        starts_macro = stripped.startswith("#define")
        macro_line = in_macro or starts_macro
        if starts_macro:
            params = macro_params(line)
        # forward declarations at global scope, consumer code only
        fm = FWD.match(line)
        if fm and not inside_engine and not macro_line:
            kind, name = fm.group(2), fm.group(3)
            new = MOVED.get(name) or (name if name in QUALIFY else None)
            if new:
                out_lines.append(f"{fm.group(1)}namespace Deki {{ {kind} {new}; }}" + (" " + fm.group(4) if fm.group(4) else ""))
                stats[name] = stats.get(name, 0) + 1
                continue
        if stripped.startswith("namespace Deki {"):
            out_lines.append(line)  # an already-migrated forward declaration
            continue
        out_lines.append(process_code(line, inside_engine, macro_line, stats, params if macro_line else ()))
        in_macro = macro_line and line.rstrip().endswith("\\")
    new_raw = nl.join(out_lines)
    changed = new_raw != raw
    if changed and not dry_run:
        open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(new_raw)
    return changed, stats


def iter_files(roots):
    for root in roots:
        if os.path.isfile(root):
            yield root
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for f in filenames:
                if os.path.splitext(f)[1] in EXTS:
                    yield os.path.join(dirpath, f)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--engine", action="store_true", help="sources inside namespace Deki: unqualified new names")
    ap.add_argument("--consumer-files", default="", help="comma-separated basenames to treat as consumer code even under --engine")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--comments", action="store_true", help="also rename type names mentioned in comments")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)
    consumer_names = {s for s in a.consumer_files.split(",") if s}
    global COMMENTS
    COMMENTS = a.comments

    total_files = 0
    total = {}
    for path in iter_files(a.paths):
        inside = a.engine and os.path.basename(path) not in consumer_names
        changed, stats = process_file(path, inside, a.dry_run)
        if changed:
            total_files += 1
            for k, v in stats.items():
                total[k] = total.get(k, 0) + v
            if not a.quiet:
                print(f"{'would change' if a.dry_run else 'changed'}: {path} ({sum(stats.values())})")
    print(f"{total_files} file(s) {'would change' if a.dry_run else 'changed'}; top names: "
          + ", ".join(f"{k}={v}" for k, v in sorted(total.items(), key=lambda kv: -kv[1])[:12]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
