#!/usr/bin/env python3
"""Turn a `DekiEditor --dump-metadata` JSON file into Markdown pages.

Two outputs, one build:

  main site (<out-dir>)        index (one link per package), engine core, project
                               settings; the Doxygen API reference is added by the
                               Doxyfile. Nothing package-specific lives here.
  package sites (--packages-out <dir>)
                               <dir>/<package-id>/index.md: that package's
                               components, properties and settings sections. Each
                               is published on the package's own repository
                               (GitHub Pages), which the registry's `docs` field
                               names and the main index links to.

    gen_component_docs.py <metadata.json> <out-dir> [--packages-out <dir>]
                          [--packages <dir>] [--registry <index.json>] [--main-url <url>]

`--packages` points at a directory of package checkouts (packages/deki-*) whose
package.json and README supply summaries, dependencies and feature flags.
`--registry` supplies the same from a registry index, plus each package's
`docs` URL and repository.
"""

import argparse
import json
import os
import re
import sys

MAIN_URL = "https://dekiengine.github.io/docs/"
PAGES_HOST = "https://dekiengine.github.io/"


# ----------------------------------------------------------------- helpers

def esc(text):
    """Escape prose for a Doxygen Markdown table cell.

    Doxygen reads `<...>` as HTML (a tooltip saying `px<->m` lost everything
    after the `<`), `@` and backslash as commands, `#word` as an autolink and
    `*` as emphasis; `|` ends the cell and a newline ends the row.
    """
    if text is None:
        return ""
    text = str(text)
    text = text.replace("\\", "\\\\").replace("@", "\\@").replace("#", "\\#").replace("*", "\\*")
    text = text.replace("<", "&lt;").replace(">", "&gt;")
    text = text.replace("|", "\\|").replace("\n", " ")
    return text


def code(text):
    return "`" + str(text).replace("`", "'") + "`"


def page_id(kind, name):
    slug = re.sub(r"[^A-Za-z0-9]+", "-", str(name)).strip("-").lower()
    return f"{kind}-{slug}"


def fmt_type(p):
    t = p.get("type", "")
    if t == "Array":
        return f"Array<{p.get('elementType', '?')}>"
    if t == "AssetRef" and p.get("assetType"):
        return f"AssetRef ({p['assetType']})"
    if t == "ObjectRef" and p.get("componentRefType"):
        return f"ObjectRef ({p['componentRefType']})"
    return t


def fmt_default(p):
    if "default" not in p:
        return ""
    v = p["default"]
    t = p.get("type")
    if t == "Color" and isinstance(v, int):
        return code(f"0x{v & 0xFFFFFFFF:08X}")
    if t == "enum" and isinstance(v, int) and p.get("enumValues"):
        names = p["enumValues"]
        if 0 <= v < len(names):
            return code(names[v])
    if isinstance(v, str):
        if v == "":
            return code('""')
        return code(json.dumps(v)[:60])
    if isinstance(v, bool):
        return code("true" if v else "false")
    if isinstance(v, float):
        return code(f"{v:.6g}")
    if isinstance(v, (dict, list)):
        s = json.dumps(v)
        return code(s if len(s) <= 60 else s[:57] + "...")
    return code(v)


def fmt_constraints(p):
    """Everything that bounds or shapes the value: enum values, range, unit, visibility, alias."""
    parts = []
    if p.get("enumValues"):
        parts.append(", ".join(code(n) for n in p["enumValues"]))
    if p.get("range"):
        r = p["range"]
        lo, hi = r.get("min", 0), r.get("max", 0)
        parts.append(f"{lo:g} to {hi:g}" + (" (slider)" if r.get("slider") else ""))
    if p.get("unit"):
        parts.append(f"in {p['unit']}")
    if p.get("visibleWhen"):
        parts.append(f"shown when {code(p['visibleWhen'].get('property'))} allows")
    if p.get("multiline"):
        parts.append("multi-line")
    if p.get("renamedFrom"):
        parts.append(f"formerly {code(p['renamedFrom'])}")
    return "; ".join(parts)


def fmt_description(p):
    return esc(p.get("tooltip", ""))


def first_paragraph(path):
    """First prose paragraph of a README (skipping the title and 'Part of' line)."""
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError:
        return ""
    para, started = [], False
    for line in lines:
        s = line.strip()
        if not started:
            if not s or s.startswith("#") or s.lower().startswith("part of") or s.lower().startswith("documentation:"):
                continue
            started = True
        if started and not s:
            break
        para.append(s)
    return " ".join(para)


def read_package_info(pkg_id, modules_dir, registry):
    """package.json + README + registry entry for a package."""
    info = {"displayName": pkg_id, "summary": "", "requires": [], "features": [], "repo": "", "docs": ""}
    if registry:
        for entry in registry.get("packages", []):
            if entry.get("id") == pkg_id:
                info["displayName"] = entry.get("displayName") or pkg_id
                info["summary"] = entry.get("description") or ""
                info["requires"] = entry.get("requires") or []
                info["repo"] = entry.get("repo") or ""
                info["latest"] = entry.get("latest") or ""
                info["docs"] = entry.get("docs") or ""
                break
    if modules_dir:
        pdir = os.path.join(modules_dir, pkg_id)
        pj = os.path.join(pdir, "package.json")
        if os.path.isfile(pj):
            try:
                with open(pj, encoding="utf-8") as f:
                    j = json.load(f)
                info["displayName"] = j.get("displayName") or info["displayName"]
                info["requires"] = j.get("requires") or info["requires"]
                info["features"] = j.get("features") or []
                info["minEngine"] = j.get("minEngine") or ""
            except (OSError, ValueError):
                pass
        readme = os.path.join(pdir, "README.md")
        if os.path.isfile(readme) and not info["summary"]:
            info["summary"] = first_paragraph(readme)
    if not info["repo"]:
        info["repo"] = f"dekiengine/{pkg_id}"
    if not info["docs"]:
        info["docs"] = docs_url_for_repo(info["repo"])
    return info


def docs_url_for_repo(repo):
    """The GitHub Pages URL of a package repository: <owner>.github.io/<name>/."""
    owner, _, name = repo.partition("/")
    if not name:
        return PAGES_HOST + owner + "/"
    return f"https://{owner}.github.io/{name}/"


# ----------------------------------------------------------------- sections

def component_section(c, level="##", pid="", base_links=None):
    """One component: heading (class name only, so the sidebar stays readable),
    description, facts, its own properties, and one line for inherited ones."""
    out = []
    name = c.get("name", "")
    anchor = page_id(pid or "component", name)
    out.append(f"{level} {name} {{#{anchor}}}")
    out.append("")
    if c.get("description"):
        out.append(esc(c["description"]))
        out.append("")
    facts = []
    display = c.get("displayName") or name
    if display and display != name:
        facts.append(f"Inspector name: {esc(display)}")
    if c.get("category"):
        facts.append(f"Category: {esc(c['category'])}")
    if c.get("guid"):
        facts.append(f"Type GUID: {code(c['guid'])}")
    flags = []
    if c.get("isAbstract"):
        flags.append("abstract")
    if c.get("isUnique"):
        flags.append("one per object")
    if c.get("bootSceneOnly"):
        flags.append("boot scene only")
    if c.get("platformSceneOnly"):
        flags.append("platform scene only")
    if flags:
        facts.append("Flags: " + ", ".join(flags))
    if facts:
        out.append(" · ".join(facts))
        out.append("")

    own = [p for p in c.get("properties", []) if not p.get("inheritedFrom")]
    inherited = [p for p in c.get("properties", []) if p.get("inheritedFrom")]
    if own:
        out.append("| Property | Type | Default | Constraints | Description |")
        out.append("| --- | --- | --- | --- | --- |")
        for p in own:
            out.append(f"| {code(p.get('name', ''))} | {esc(fmt_type(p))} | {fmt_default(p)} | "
                       f"{fmt_constraints(p)} | {fmt_description(p)} |")
        out.append("")
    elif not inherited:
        out.append("No reflected properties.")
        out.append("")
    if inherited:
        base = c.get("base") or inherited[0].get("inheritedFrom", "")
        link = (base_links or {}).get(base)
        base_text = f"[{base}]({link})" if link else base
        names = ", ".join(code(p.get("name", "")) for p in inherited)
        out.append(f"Inherits {names} from {base_text}.")
        out.append("")
    return out


def settings_section(s, pid, level="##", base_links=None):
    c = dict(s)
    c["name"] = f"{s.get('section', '')} settings" if s.get("section") else s.get("name", "")
    c["displayName"] = c["name"]
    c["description"] = s.get("description") or f"Project settings section `{s.get('section', '')}` ({s.get('name', '')})."
    return component_section(c, level, pid, base_links)


def write(path, lines):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    return path


# ----------------------------------------------------------------- where each component lives

def component_links(data, infos, main_url):
    """Component class name -> URL of its documentation (its package's site, or
    the main site's engine page), so inherited-property lines can link to the base."""
    links = {}
    for c in data.get("engine", {}).get("components", []):
        links[c.get("name", "")] = f"{main_url}engine-core.html#{page_id('engine', c.get('name', ''))}"
    for pkg in data.get("packages", []):
        site = infos.get(pkg["id"], {}).get("docs") or docs_url_for_repo(f"dekiengine/{pkg['id']}")
        for c in pkg.get("components", []):
            links[c.get("name", "")] = f"{site}#{page_id('c', c.get('name', ''))}"
    return links


def settings_type_names(data):
    """Class names registered as project-settings sections; they are components
    too, but belong on the settings pages, not in a components list."""
    names = set()
    for sec in data.get("engine", {}).get("settings", []):
        names.add(sec.get("name", ""))
    for pkg in data.get("packages", []):
        for sec in pkg.get("settings", []):
            names.add(sec.get("name", ""))
    return names


# ----------------------------------------------------------------- package site

def package_site_page(pkg, info, infos, main_url, version, base_links=None, skip_names=()):
    """The single page of a package's own docs site."""
    pkg_id = pkg["id"]
    display = info.get("displayName") or pkg_id
    out = [f"# {pkg_id} {{#mainpage}}", ""]
    if info.get("summary"):
        out.append(esc(info["summary"]))
        out.append("")
    facts = [f"Repository: [github.com/{info['repo']}](https://github.com/{info['repo']})"]
    if info.get("latest"):
        facts.append(f"Latest release: {info['latest']}")
    if info.get("minEngine"):
        facts.append(f"Needs engine {info['minEngine']}+")
    if info.get("requires"):
        links = []
        for r in info["requires"]:
            url = infos.get(r, {}).get("docs") or docs_url_for_repo(f"dekiengine/{r}")
            links.append(f"[{r}]({url})")
        facts.append("Requires: " + ", ".join(links))
    out.append(" · ".join(facts))
    out.append("")
    out.append(f"Install: `DekiEditor --packages-add {pkg_id} <project>`, or add it in the Package Manager. "
               f"Part of [Deki]({main_url}); the engine's own components and the C++ API are documented there.")
    out.append("")
    if display != pkg_id:
        out.append(f"Shown in the editor as **{display}**.")
        out.append("")

    comps = [c for c in pkg.get("components", []) if c.get("name") not in skip_names]
    settings = pkg.get("settings", [])
    if comps:
        out.append("## Components")
        out.append("")
        out.append("| Component | Category | Description |")
        out.append("| --- | --- | --- |")
        for c in comps:
            out.append(f"| [{c.get('name', '')}](#{page_id('c', c.get('name', ''))}) | "
                       f"{esc(c.get('category', ''))} | {esc(c.get('description', ''))} |")
        out.append("")
    if info.get("features"):
        out.append("## Feature flags")
        out.append("")
        out.append("Compile-time features the package exposes (`package.json`):")
        out.append("")
        out.append("| Feature | Define | Default |")
        out.append("| --- | --- | --- |")
        for feat in info["features"]:
            out.append(f"| {esc(feat.get('displayName') or feat.get('id'))} | {code(feat.get('buildDefine', ''))} | "
                       f"{'on' if feat.get('defaultEnabled', True) else 'off'} |")
        out.append("")
    if comps:
        out.append("## Component reference")
        out.append("")
        for c in comps:
            out.extend(component_section(c, "###", "c", base_links))
    if settings:
        out.append("## Project settings")
        out.append("")
        for sec in settings:
            out.extend(settings_section(sec, "s", "###", base_links))
    if not comps and not settings:
        out.append("This package registers no components; it provides a driver or a library the")
        out.append("engine calls into (see its README).")
        out.append("")
    out.append("---")
    out.append("")
    out.append(f"Generated from the package's reflection metadata by the Deki documentation build"
               + (f" (engine {version})." if version else "."))
    out.append("Reflected property names are the keys scene files use; a rename ships with an alias,")
    out.append(f"see [COMPATIBILITY.md](https://github.com/dekiengine/deki-editor/blob/master/COMPATIBILITY.md).")
    out.append("")
    return out


# ----------------------------------------------------------------- main site

def engine_page(engine, base_links=None, skip_names=()):
    out = ["# Engine core {#engine-core}", "",
           "Components and settings sections the engine itself provides, available in every",
           "project without any package.", ""]
    comps = [c for c in engine.get("components", []) if c.get("name") not in skip_names]
    if comps:
        out.append("## Components")
        out.append("")
        out.append("| Component | Category | Description |")
        out.append("| --- | --- | --- |")
        for c in comps:
            out.append(f"| [{c.get('name', '')}](#{page_id('engine', c.get('name', ''))}) | "
                       f"{esc(c.get('category', ''))} | {esc(c.get('description', ''))} |")
        out.append("")
        out.append("## Component reference")
        out.append("")
        for c in comps:
            out.extend(component_section(c, "###", "engine", base_links))
    settings = engine.get("settings", [])
    if settings:
        out.append("## Project settings")
        out.append("")
        for sec in settings:
            out.extend(settings_section(sec, "engine", "###", base_links))
    return out


def settings_page(data, infos):
    out = ["# Project settings {#settings}", "",
           "Every settings section a project can carry under `settings/`, by the package that",
           "defines it. Values are reflected properties: the key in the file is the property name.", ""]
    rows = []
    for s in data.get("engine", {}).get("settings", []):
        rows.append(("engine", s))
    for pkg in data.get("packages", []):
        for s in pkg.get("settings", []):
            rows.append((pkg["id"], s))
    if not rows:
        out.append("No settings sections were registered.")
        return out
    out.append("| Section | Defined by | Properties |")
    out.append("| --- | --- | --- |")
    for owner, s in rows:
        n = len(s.get("properties", []))
        if owner == "engine":
            target = "engine-core.html"
        else:
            target = infos.get(owner, {}).get("docs") or docs_url_for_repo(f"dekiengine/{owner}")
        out.append(f"| {esc(s.get('section', ''))} | [{owner}]({target}) | {n} |")
    out.append("")
    engine_rows = [s for owner, s in rows if owner == "engine"]
    if engine_rows:
        out.append("## Engine sections")
        out.append("")
        for sec in engine_rows:
            out.extend(settings_section(sec, "settings", "###"))
    out.append("Sections defined by packages are documented on each package's own site (linked above).")
    out.append("")
    return out


def index_page(data, infos):
    version = data.get("engineVersion", "")
    out = ["# Deki {#mainpage}", ""]
    out.append("A modular C++ 2D engine for embedded displays and desktop, with a visual editor.")
    out.append("This site documents the engine core and its C++ API; every package documents itself"
               " on its own site, linked below. All of it is generated from the code"
               + (f" (engine {version})." if version else "."))
    out.append("")
    out.append("- [Engine core](engine-core.html): components and settings the engine provides itself")
    out.append("- [Project settings](settings.html): every settings section, by owner")
    out.append("- [API reference](annotated.html): the C++ classes of the engine and the editor DLL")
    out.append("- [Editor repository](https://github.com/dekiengine/deki-editor): setup, contributing, compatibility policy")
    out.append("")
    out.append("## Packages {#packages}")
    out.append("")
    out.append("Each package is its own repository under [github.com/dekiengine](https://github.com/dekiengine)"
               " and is installed with `DekiEditor --packages-add <id> <project>` or the Package Manager.")
    out.append("")
    out.append("| Package | Components | Summary |")
    out.append("| --- | --- | --- |")
    for pkg in data.get("packages", []):
        pkg_id = pkg["id"]
        info = infos.get(pkg_id, {})
        n = len(pkg.get("components", []))
        out.append(f"| [{pkg_id}]({info.get('docs', '')}) | {n} | {esc(info.get('summary', ''))} |")
    out.append("")
    return out


# ----------------------------------------------------------------- driver

def generate(metadata_path, out_dir, modules_dir=None, registry_path=None, packages_out=None, main_url=MAIN_URL):
    with open(metadata_path, encoding="utf-8") as f:
        data = json.load(f)
    registry = None
    if registry_path and os.path.isfile(registry_path):
        with open(registry_path, encoding="utf-8") as f:
            registry = json.load(f)
    version = data.get("engineVersion", "")
    packages = data.get("packages", [])
    infos = {pkg["id"]: read_package_info(pkg["id"], modules_dir, registry) for pkg in packages}

    base_links = component_links(data, infos, main_url)
    skip = settings_type_names(data)

    written = []
    os.makedirs(out_dir, exist_ok=True)
    written.append(write(os.path.join(out_dir, "index.md"), index_page(data, infos)))
    written.append(write(os.path.join(out_dir, "engine.md"), engine_page(data.get("engine", {}), base_links, skip)))
    written.append(write(os.path.join(out_dir, "settings.md"), settings_page(data, infos)))

    if packages_out:
        for pkg in packages:
            page = package_site_page(pkg, infos[pkg["id"]], infos, main_url, version, base_links, skip)
            written.append(write(os.path.join(packages_out, pkg["id"], "index.md"), page))
    return written


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("metadata")
    ap.add_argument("out_dir", help="Markdown for the main site")
    ap.add_argument("--packages-out", help="one <id>/index.md per package, for the package sites")
    ap.add_argument("--packages", help="directory holding package checkouts (packages/deki-*)")
    ap.add_argument("--registry", help="registry index.json (summaries, repos, docs URLs)")
    ap.add_argument("--main-url", default=MAIN_URL, help="URL of the main site, linked from package sites")
    a = ap.parse_args(argv)
    for p in generate(a.metadata, a.out_dir, a.packages, a.registry, a.packages_out, a.main_url):
        print(p)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
