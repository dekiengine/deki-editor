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

`--language ja|zh|ko` writes the same pages in that language, for the site's
/<code>/ folder; `--guides` adds the editor guides (deki-editor/docs) as pages.
See localize.py for where each kind of text comes from.
"""

import argparse
import json
import os
import re
import sys

import localize

MAIN_URL = "https://dekiengine.github.io/docs/"
PAGES_HOST = "https://dekiengine.github.io/"

# The language being written. generate() sets it; every page function reads it.
L = localize.Lang()


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
    r"""Inline code for a Markdown table cell, or an em dash for nothing.

    An empty value must not come back as a bare pair of backticks. Doxygen
    reads that as a code span that never closes and swallows the rest of the
    table into it, so the page shows one cell containing the raw `</td>` and
    `\ilinebr` of every row after it. deki-sdl3-integration's two features
    have no buildDefine -- they are stripped by their component types rather
    than by a define -- and that was enough to destroy its whole table.
    """
    if text is None:
        return EMPTY_CELL
    s = str(text)
    if not s.strip():
        return EMPTY_CELL
    return "`" + s.replace("`", "'") + "`"


# What a table cell shows when there is nothing to show.
EMPTY_CELL = "&mdash;"


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
        lo, hi = f"{r.get('min', 0):g}", f"{r.get('max', 0):g}"
        parts.append(L.ui("{lo} to {hi} (slider)" if r.get("slider") else "{lo} to {hi}", lo=lo, hi=hi))
    if p.get("unit"):
        parts.append(L.ui("in {unit}", unit=p["unit"]))
    if p.get("visibleWhen"):
        parts.append(L.ui("shown when {property} allows", property=code(p["visibleWhen"].get("property"))))
    if p.get("multiline"):
        parts.append(L.ui("multi-line"))
    if p.get("renamedFrom"):
        parts.append(L.ui("formerly {name}", name=code(p["renamedFrom"])))
    return L.ui("; ").join(parts)


def fmt_description(p, package=None):
    return esc(L.tr(p.get("tooltip", ""), package))


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


def read_changelog(path, versions=2):
    """The most recent version sections of a package's CHANGELOG.md.

    The whole file goes on the site once a package has years of history, which
    buries the page the reader came for. Two versions is what "what changed
    for me" needs; the rest is one link away on GitHub.
    """
    if not os.path.isfile(path):
        return ""
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError:
        return ""

    out, seen = [], 0
    for line in lines:
        if line.startswith("## "):
            seen += 1
            if seen > versions:
                break
        if seen:
            out.append(line)
    return chr(10).join(out).strip()


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
        info["changelog"] = read_changelog(os.path.join(pdir, "CHANGELOG.md"))
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

def property_table(props, package=None):
    """A property table with the empty columns left out.

    Constraints and Description are only there when someone wrote them, and
    most components have neither: Description was entirely blank in 24 of the
    33 tables across the package docs and Constraints in 5. A column of empty
    cells is not neutral, it is width taken from the three that say something,
    on the page a reader came to for exactly those three.
    """
    columns = [
        ("Property", lambda p: code(p.get("name", ""))),
        ("Type", lambda p: esc(fmt_type(p))),
        ("Default", lambda p: fmt_default(p)),
        ("Constraints", fmt_constraints),
        ("Description", lambda p: fmt_description(p, package)),
    ]
    cells = {name: [render(p) for p in props] for name, render in columns}
    kept = [name for name, _ in columns if any(c.strip() for c in cells[name])]

    out = ["| " + " | ".join(L.ui(name) for name in kept) + " |",
           "| " + " | ".join("---" for _ in kept) + " |"]
    for i in range(len(props)):
        out.append("| " + " | ".join(cells[name][i] for name in kept) + " |")
    out.append("")
    return out


def component_section(c, level="##", pid="", base_links=None, package=None, anchor_name=None):
    """One component: heading (class name only, so the sidebar stays readable),
    description, facts, its own properties, and one line for inherited ones."""
    out = []
    name = c.get("name", "")
    anchor = page_id(pid or "component", anchor_name or name)
    out.append(f"{level} {name} {{#{anchor}}}")
    out.append("")
    if c.get("description"):
        out.append(esc(L.tr(c["description"], package)))
        out.append("")
    facts = []
    display = c.get("displayName") or name
    if display and display != name:
        facts.append(L.ui("Inspector name: {name}", name=esc(L.tr(display, package))))
    if c.get("category"):
        facts.append(L.ui("Category: {name}", name=esc(L.tr(c["category"], package))))
    if c.get("type"):
        facts.append(L.ui("Type: {name}", name=code(c["type"])))
    flags = []
    if c.get("isAbstract"):
        flags.append(L.ui("abstract"))
    if c.get("isUnique"):
        flags.append(L.ui("one per object"))
    if c.get("bootSceneOnly"):
        flags.append(L.ui("boot scene only"))
    if c.get("platformSceneOnly"):
        flags.append(L.ui("platform scene only"))
    if flags:
        facts.append(L.ui("Flags: {names}", names=L.ui(", ").join(flags)))
    if facts:
        out.append(" · ".join(facts))
        out.append("")

    own = [p for p in c.get("properties", []) if not p.get("inheritedFrom")]
    inherited = [p for p in c.get("properties", []) if p.get("inheritedFrom")]
    if own:
        out.extend(property_table(own, package))
    elif not inherited:
        out.append(L.ui("No reflected properties."))
        out.append("")
    if inherited:
        base = c.get("base") or inherited[0].get("inheritedFrom", "")
        link = (base_links or {}).get(base)
        base_text = f"[{base}]({link})" if link else base
        names = ", ".join(code(p.get("name", "")) for p in inherited)
        out.append(L.ui("Inherits {names} from {base}.", names=names, base=base_text))
        out.append("")
    return out


def settings_section(s, pid, level="##", base_links=None, package=None):
    c = dict(s)
    section = s.get("section", "")
    c["name"] = L.ui("{section} settings", section=L.tr(section, package)) if section else s.get("name", "")
    c["displayName"] = c["name"]
    c["description"] = s.get("description") or L.ui("Project settings section {section} ({name}).",
                                                    section=code(section), name=s.get("name", ""))
    # The anchor is the English one in every language, so links between them line up.
    anchor_name = f"{section} settings" if section else s.get("name", "")
    return component_section(c, level, pid, base_links, package, anchor_name)


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


# Category artwork shared with the website, one PNG per package id. Generated
# by hugo-dekiengine/tools/make-package-covers.py and copied in; Doxyfile.package
# points IMAGE_PATH here.
COVERS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "covers")
COVERS = __import__("pathlib").Path(COVERS)


# ----------------------------------------------------------------- package site

def package_site_page(pkg, info, infos, main_url, version, base_links=None, skip_names=()):
    """The single page of a package's own docs site."""
    pkg_id = pkg["id"]
    display = info.get("displayName") or pkg_id
    out = [f"# {pkg_id} {{#mainpage}}", ""]
    # The same small drawing the website uses for this package, if one exists.
    # Doxygen resolves it through IMAGE_PATH and copies it into the output, so
    # a package with no cover simply has no picture rather than a broken one.
    # The theme keeps it small; it is a label, not a figure.
    if (COVERS / f"{pkg_id}.png").is_file():
        out.append(f"![]({pkg_id}.png)")
        out.append("")
    summary = L.package_summary(pkg_id) or info.get("summary")
    if summary:
        out.append(esc(summary))
        out.append("")
    repo_link = f"[github.com/{info['repo']}](https://github.com/{info['repo']})"
    facts = [L.ui("Repository: {link}", link=repo_link)]
    if info.get("latest"):
        facts.append(L.ui("Latest release: {version}", version=info["latest"]))
    if info.get("minEngine"):
        facts.append(L.ui("Needs engine {version}+", version=info["minEngine"]))
    if info.get("requires"):
        links = []
        for r in info["requires"]:
            url = infos.get(r, {}).get("docs") or docs_url_for_repo(f"dekiengine/{r}")
            links.append(f"[{r}]({url})")
        facts.append(L.ui("Requires: {names}", names=", ".join(links)))
    out.append(" · ".join(facts))
    out.append("")
    out.append(L.ui("Install with {command}, or from the Package Manager. The engine's own components "
                    "and C++ API are in the [Deki docs]({url}).",
                    command=f"`DekiEditor --packages-add {pkg_id} <project>`", url=main_url))
    out.append("")
    if display != pkg_id:
        out.append(L.ui("Shown in the editor as **{name}**.", name=display))
        out.append("")

    comps = [c for c in pkg.get("components", []) if c.get("name") not in skip_names]
    settings = pkg.get("settings", [])
    if comps:
        out.append(f"## {L.ui('Components')} {{#components}}")
        out.append("")
        out.append(f"| {L.ui('Component')} | {L.ui('Category')} | {L.ui('Description')} |")
        out.append("| --- | --- | --- |")
        for c in comps:
            out.append(f"| [{c.get('name', '')}](#{page_id('c', c.get('name', ''))}) | "
                       f"{esc(L.tr(c.get('category', ''), pkg_id))} | "
                       f"{esc(L.tr(c.get('description', ''), pkg_id))} |")
        out.append("")
    if info.get("features"):
        out.append(f"## {L.ui('Feature flags')} {{#feature-flags}}")
        out.append("")
        out.append(L.ui("Parts you can leave out of a build. A feature with a define is "
                        "switched there; the rest are stripped with their components."))
        out.append("")
        out.append(f"| {L.ui('Feature')} | {L.ui('Define')} | {L.ui('Default')} |")
        out.append("| --- | --- | --- |")
        for feat in info["features"]:
            out.append(f"| {esc(feat.get('displayName') or feat.get('id'))} | "
                       f"{code(feat.get('buildDefine', ''))} | "
                       f"{L.ui('on') if feat.get('defaultEnabled', True) else L.ui('off')} |")
        out.append("")
    if comps:
        out.append(f"## {L.ui('Component reference')} {{#component-reference}}")
        out.append("")
        for c in comps:
            out.extend(component_section(c, "###", "c", base_links, pkg_id))
    if settings:
        out.append(f"## {L.ui('Project settings')} {{#project-settings}}")
        out.append("")
        for sec in settings:
            out.extend(settings_section(sec, "s", "###", base_links, pkg_id))
    if not comps and not settings:
        out.append(L.ui("This package is a driver or a library the engine calls into. Its "
                        "README has the details."))
        out.append("")
    if info.get("changelog"):
        out.append(f"## {L.ui('Changelog')} {{#changelog}}")
        out.append("")
        if not L.english:
            out.append(L.ui("The changelog is in English."))
            out.append("")
        # Demoted one level: the file's "## 0.15.0" would otherwise sit at the
        # same level as this page's own sections.
        for line in info["changelog"].splitlines():
            out.append("#" + line if line.startswith("## ") else line)
        out.append("")
        out.append(L.ui("Earlier releases: {link}.",
                        link=f"[CHANGELOG.md](https://github.com/{info['repo']}/blob/master/CHANGELOG.md)"))
        out.append("")

    out.append("---")
    out.append("")
    compatibility = "https://github.com/dekiengine/deki-editor/blob/master/COMPATIBILITY.md"
    if version:
        out.append(L.ui("Generated from the package's reflection data (engine {version}). Property names are the "
                        "keys used in scene files. Renames keep an alias ([compatibility]({url})).",
                        version=version, url=compatibility))
    else:
        out.append(L.ui("Generated from the package's reflection data. Property names are the keys used in scene "
                        "files. Renames keep an alias ([compatibility]({url})).", url=compatibility))
    out.append("")
    return out


# ----------------------------------------------------------------- main site

def engine_page(engine, base_links=None, skip_names=()):
    out = [f"# {L.ui('Engine core')} {{#engine-core}}", "",
           L.ui("Components and settings sections the engine itself provides, available in every "
                "project without any package."), ""]
    comps = [c for c in engine.get("components", []) if c.get("name") not in skip_names]
    if comps:
        out.append(f"## {L.ui('Components')} {{#engine-components}}")
        out.append("")
        out.append(f"| {L.ui('Component')} | {L.ui('Category')} | {L.ui('Description')} |")
        out.append("| --- | --- | --- |")
        for c in comps:
            out.append(f"| [{c.get('name', '')}](#{page_id('engine', c.get('name', ''))}) | "
                       f"{esc(L.tr(c.get('category', '')))} | {esc(L.tr(c.get('description', '')))} |")
        out.append("")
        out.append(f"## {L.ui('Component reference')} {{#engine-component-reference}}")
        out.append("")
        for c in comps:
            out.extend(component_section(c, "###", "engine", base_links))
    settings = engine.get("settings", [])
    if settings:
        out.append(f"## {L.ui('Project settings')} {{#engine-project-settings}}")
        out.append("")
        for sec in settings:
            out.extend(settings_section(sec, "engine", "###", base_links))
    return out


def settings_page(data, infos):
    out = [f"# {L.ui('Project settings')} {{#settings}}", "",
           L.ui("Every settings section a project can have under {folder}, and who defines it. "
                "The key in the file is the property name.", folder="`settings/`"), ""]
    rows = []
    for s in data.get("engine", {}).get("settings", []):
        rows.append(("engine", s))
    for pkg in data.get("packages", []):
        for s in pkg.get("settings", []):
            rows.append((pkg["id"], s))
    if not rows:
        out.append(L.ui("No settings sections."))
        return out
    out.append(f"| {L.ui('Section')} | {L.ui('Defined by')} | {L.ui('Properties')} |")
    out.append("| --- | --- | --- |")
    for owner, s in rows:
        n = len(s.get("properties", []))
        if owner == "engine":
            target = "engine-core.html"
        else:
            target = infos.get(owner, {}).get("docs") or docs_url_for_repo(f"dekiengine/{owner}")
        package = None if owner == "engine" else owner
        out.append(f"| {esc(L.tr(s.get('section', ''), package))} | [{owner}]({target}) | {n} |")
    out.append("")
    engine_rows = [s for owner, s in rows if owner == "engine"]
    if engine_rows:
        out.append(f"## {L.ui('Engine sections')} {{#engine-sections}}")
        out.append("")
        for sec in engine_rows:
            out.extend(settings_section(sec, "settings", "###"))
    out.append(L.ui("Package sections are documented on each package's own site, linked above."))
    out.append("")
    return out


def index_page(data, infos, guides=(), api_url="annotated.html"):
    version = data.get("engineVersion", "")
    out = ["# Deki {#mainpage}", ""]
    out.append(L.ui("A modular C++ 2D engine for embedded displays and desktop, with a visual editor."))
    out.append("")
    if version:
        out.append(L.ui("The engine core and its C++ API are here. Packages document themselves, linked below. "
                        "All of it is generated from the code (engine {version}).", version=version))
    else:
        out.append(L.ui("The engine core and its C++ API are here. Packages document themselves, linked below. "
                        "All of it is generated from the code."))
    out.append("")
    out.append(L.ui("- [Engine core]({url}): components and settings the engine provides itself",
                    url="engine-core.html"))
    out.append(L.ui("- [Project settings]({url}): every settings section, by owner", url="settings.html"))
    if L.english:
        out.append(L.ui("- [API reference]({url}): the C++ classes of the engine and the editor DLL", url=api_url))
    else:
        out.append(L.ui("- [API reference]({url}): the C++ classes of the engine and the editor DLL, in English",
                        url=api_url))
    out.append(L.ui("- [Editor repository]({url}): setup, contributing, compatibility policy",
                    url="https://github.com/dekiengine/deki-editor"))
    out.append("")
    if guides:
        out.append(f"## {L.ui('Guides')} {{#guides}}")
        out.append("")
        for name, title in guides:
            out.append(f"- [{title}](guide-{name}.html)")
        out.append("")
    out.append(f"## {L.ui('Packages')} {{#packages}}")
    out.append("")
    out.append(L.ui("Each is its own repository under {link}. Install with {command}, or from the Package Manager.",
                    link="[github.com/dekiengine](https://github.com/dekiengine)",
                    command="`DekiEditor --packages-add <id> <project>`"))
    out.append("")
    out.append(f"| {L.ui('Package')} | {L.ui('Components')} | {L.ui('Summary')} |")
    out.append("| --- | --- | --- |")
    for pkg in data.get("packages", []):
        pkg_id = pkg["id"]
        info = infos.get(pkg_id, {})
        n = len(pkg.get("components", []))
        summary = L.package_summary(pkg_id) or info.get("summary", "")
        out.append(f"| [{pkg_id}]({info.get('docs', '')}) | {n} | {esc(summary)} |")
    out.append("")
    return out


def guide_pages(guides_dir, out_dir):
    """Write every guide in deki-editor/docs as a page; returns [(name, title)].

    A guide with no translation yet is shown in English under a note saying so.
    """
    names = sorted(f[:-3] for f in os.listdir(guides_dir) if f.endswith(".md"))
    found = []
    for name in names:
        with open(os.path.join(guides_dir, name + ".md"), encoding="utf-8") as f:
            english = f.read()
        text, depth = english, 0
        translated = os.path.join(guides_dir, L.code, name + ".md")
        if not L.english and os.path.isfile(translated):
            with open(translated, encoding="utf-8") as f:
                text, depth = f.read(), 1
        page = localize.guide_page(text, name, names, english, depth)
        if not L.english and depth == 0:
            first, _, rest = page.partition("\n")
            page = f"{first}\n\n{L.ui('This guide is not translated yet.')}\n{rest}"
        write(os.path.join(out_dir, f"guide-{name}.md"), page.splitlines())
        found.append((name, localize.guide_title(text)))
    return found


# ----------------------------------------------------------------- driver

def language_url(url, language):
    """A site's URL in a language: English at the root, the others in /<code>/."""
    return url if language == "en" else url.rstrip("/") + f"/{language}/"


def generate(metadata_path, out_dir, modules_dir=None, registry_path=None, packages_out=None, main_url=MAIN_URL,
             language="en", editor_root=None, site_dir=None, guides_dir=None):
    global L
    L = localize.Lang(language, editor_root, modules_dir, site_dir)
    with open(metadata_path, encoding="utf-8") as f:
        data = json.load(f)
    registry = None
    if registry_path and os.path.isfile(registry_path):
        with open(registry_path, encoding="utf-8") as f:
            registry = json.load(f)
    version = data.get("engineVersion", "")
    packages = data.get("packages", [])
    infos = {pkg["id"]: read_package_info(pkg["id"], modules_dir, registry) for pkg in packages}
    for info in infos.values():
        info["docs"] = language_url(info["docs"], language)
    api_url = "annotated.html" if L.english else main_url + "annotated.html"
    main_url = language_url(main_url, language)

    base_links = component_links(data, infos, main_url)
    skip = settings_type_names(data)

    written = []
    os.makedirs(out_dir, exist_ok=True)
    guides = guide_pages(guides_dir, out_dir) if guides_dir else []
    written.extend(os.path.join(out_dir, f"guide-{name}.md") for name, _ in guides)
    written.append(write(os.path.join(out_dir, "index.md"), index_page(data, infos, guides, api_url)))
    written.append(write(os.path.join(out_dir, "engine.md"), engine_page(data.get("engine", {}), base_links, skip)))
    written.append(write(os.path.join(out_dir, "settings.md"), settings_page(data, infos)))

    if packages_out:
        for pkg in packages:
            page = package_site_page(pkg, infos[pkg["id"]], infos, main_url, version, base_links, skip)
            written.append(write(os.path.join(packages_out, pkg["id"], "index.md"), page))
    L.report()
    return written


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("metadata")
    ap.add_argument("out_dir", help="Markdown for the main site")
    ap.add_argument("--packages-out", help="one <id>/index.md per package, for the package sites")
    ap.add_argument("--packages", help="directory holding package checkouts (packages/deki-*)")
    ap.add_argument("--registry", help="registry index.json (summaries, repos, docs URLs)")
    ap.add_argument("--main-url", default=MAIN_URL, help="URL of the main site, linked from package sites")
    ap.add_argument("--language", default="en", choices=sorted(localize.LABELS), help="language of the pages")
    ap.add_argument("--guides", help="the editor guides to add as pages (deki-editor/docs)")
    ap.add_argument("--editor-root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."),
                    help="deki-editor checkout, for its translation tables")
    ap.add_argument("--site", help="the website checkout (hugo-dekiengine), for translated package summaries")
    a = ap.parse_args(argv)
    for p in generate(a.metadata, a.out_dir, a.packages, a.registry, a.packages_out, a.main_url,
                      a.language, a.editor_root, a.site, a.guides):
        print(p)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
