"""Turns Doxygen's class list into the page the API reference tab opens on.

Doxygen draws a collapsible tree of namespaces with a letter icon on each row.
This keeps its page and replaces the table: the classes only, in alphabetical
order, one row each, under the part of Deki they belong to. deki.css draws it.
"""

import html
import re

ROW = re.compile(r'<tr id="row_([\d_]+)"[^>]*><td class="entry">(.*?)</td><td class="desc">(.*?)</td></tr>', re.S)
LINK = re.compile(r'<a class="el" href="([^"]+)"[^>]*>(.*?)</a>', re.S)
ICON = re.compile(r'<span class="icon">(\w)</span>')
TABLE = re.compile(r'<div class="directory">.*?</table>\s*</div>', re.S)
TAG = re.compile(r"<[^>]+>")

GROUPS = [("Deki", "Engine"), ("DekiEditor", "Editor")]
# Where most code begins. Everything else is in the lists below it.
START = ["AssetRef", "Component", "Engine", "Object", "ObjectRef", "Scene", "Time"]


def plain(markup):
    return " ".join(html.unescape(TAG.sub("", markup)).split())


def classes(page):
    """[(top namespace, name inside it, href, description)], classes only."""
    seen = {}
    found = []
    for path, entry, desc in ROW.findall(page):
        link = LINK.search(entry)
        icon = ICON.search(entry)
        if not link or not icon:
            continue
        name = plain(link.group(2))
        parts = path.strip("_").split("_")
        seen[tuple(parts)] = name
        if icon.group(1) == "N":
            continue
        scope = [seen[tuple(parts[:i])] for i in range(1, len(parts)) if tuple(parts[:i]) in seen]
        top = scope[0] if scope else ""
        found.append((top, "::".join(scope[1:] + [name]), link.group(1), plain(desc)))
    return found


def rows(items):
    out = ['<div class="deki-api-rows">']
    for name, href, desc in items:
        out.append(f'<a class="deki-api-row" href="{html.escape(href)}"><span class="deki-api-name">{html.escape(name)}</span>'
                   f'<span class="deki-api-text">{html.escape(desc)}</span></a>')
    out.append("</div>")
    return out


def rewrite(page):
    """`page` with its table replaced. Raises if Doxygen's markup has moved on,
    rather than publishing an empty list."""
    found = classes(page)
    if not found or not TABLE.search(page):
        raise SystemExit("class_list: annotated.html does not look like Doxygen's class list any more")

    by_name = {name: (name, href, desc) for top, name, href, desc in found if top == "Deki"}
    out = ['<div class="deki-api">', '<h2 class="deki-api-heading">Start here</h2>']
    out += rows([by_name[name] for name in START if name in by_name])
    for namespace, heading in GROUPS:
        members = sorted((item[1:] for item in found if item[0] == namespace), key=lambda item: item[0].lower())
        if members:
            out += [f'<h2 class="deki-api-heading">{html.escape(heading)}</h2>'] + rows(members)
    known = {namespace for namespace, _ in GROUPS}
    other = sorted((item[1:] for item in found if item[0] not in known), key=lambda item: item[0].lower())
    if other:
        out += ['<h2 class="deki-api-heading">Other</h2>'] + rows(other)
    out.append("</div>")
    return TABLE.sub(lambda _: "\n".join(out), page, count=1)


def rewrite_file(path):
    with open(path, encoding="utf-8", newline="") as f:
        page = f.read()
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(rewrite(page))
