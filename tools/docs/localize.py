"""Languages for the documentation site.

The docs are built once per language: English at the site root, the others
under /ja/, /zh/ and /ko/. Three kinds of text are translated:

  fixed phrases     the generator's own headings and sentences, from
                    tools/docs/lang/<code>.json (keys are the English phrase)
  component text    names, categories, descriptions and tooltips, from the same
                    tables the editor reads (deki-editor/resources/lang and each
                    package's editor/lang), so a page says what the Inspector says
  guides            deki-editor/docs/<code>/<name>.md, translated by hand

Anything without a translation stays in English. Property names, enum values,
types and code stay as written in every language: they are the keys in scene
files and in C++.
"""

import json
import os
import posixpath
import re
import sys

# Code -> the name Doxygen's OUTPUT_LANGUAGE takes. English is the default.
DOXYGEN_LANGUAGES = {"en": "English", "ja": "Japanese", "zh": "Chinese", "ko": "Korean"}

# Each language named in itself, for the language menu.
LABELS = {"en": "English", "ja": "日本語", "zh": "简体中文", "ko": "한국어"}

# Google Fonts family for the language's own characters, loaded after the site's faces.
NOTO = {"ja": "Noto+Sans+JP", "zh": "Noto+Sans+SC", "ko": "Noto+Sans+KR"}

HERE = os.path.dirname(os.path.abspath(__file__))
EDITOR_REPO = "https://github.com/dekiengine/deki-editor/blob/master/"
ENGINE_REPO = "https://github.com/dekiengine/deki-engine/blob/master/"


def _read_table(path):
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        return {}
    return {k: v for k, v in data.items() if isinstance(v, str) and v}


class Lang:
    """One language's text. English returns everything as given."""

    def __init__(self, code="en", editor_root=None, packages_dir=None, site_dir=None):
        self.code = code
        self.packages_dir = packages_dir
        self.site_dir = site_dir
        self.phrases, self.shared, self.package_tables = {}, {}, {}
        self.missing, self.untranslated = set(), set()
        if code == "en":
            return
        self.phrases = _read_table(os.path.join(HERE, "lang", f"{code}.json"))
        if editor_root:
            lang_dir = os.path.join(editor_root, "resources", "lang")
            self.shared.update(_read_table(os.path.join(lang_dir, f"{code}.json")))
            sub = os.path.join(lang_dir, code)
            if os.path.isdir(sub):
                for name in sorted(os.listdir(sub)):
                    if name.endswith(".json"):
                        self.shared.update(_read_table(os.path.join(sub, name)))

    @property
    def english(self):
        return self.code == "en"

    def ui(self, text, **values):
        """A fixed phrase of the generator, with {name} placeholders filled in."""
        if not self.english:
            if text in self.phrases:
                text = self.phrases[text]
            else:
                self.missing.add(text)
        return text.format(**values) if values else text

    def tr(self, text, package=None):
        """Component text, looked up the way the editor does: the package's own
        table first, then the editor's."""
        if not text or self.english:
            return text
        if package:
            table = self._package_table(package)
            if text in table:
                return table[text]
        if text in self.shared:
            return self.shared[text]
        if not package and self.packages_dir and os.path.isdir(self.packages_dir):
            # The metadata lists some package classes with the engine's, such
            # as deki-rendering's RendererComponent, every renderer's base.
            for other in sorted(os.listdir(self.packages_dir)):
                table = self._package_table(other)
                if text in table:
                    return table[text]
        self.untranslated.add((package or "engine", text))
        return text

    def _package_table(self, package):
        if package not in self.package_tables:
            table = {}
            if self.packages_dir:
                table = _read_table(os.path.join(self.packages_dir, package, "editor", "lang", f"{self.code}.json"))
            self.package_tables[package] = table
        return self.package_tables[package]

    def package_summary(self, package):
        """The package's one-line summary from the website's page for it, or ""."""
        if self.english or not self.site_dir:
            return ""
        slug = package[5:] if package.startswith("deki-") else package
        path = os.path.join(self.site_dir, "content", "packages", f"{slug}.{self.code}.md")
        try:
            with open(path, encoding="utf-8") as f:
                text = f.read()
        except OSError:
            return ""
        m = re.search(r'^summary:\s*(.+)$', text, re.M)
        if not m:
            return ""
        value = m.group(1).strip()
        if value.startswith('"'):
            try:
                return json.loads(value)
            except ValueError:
                return value.strip('"')
        return value.strip("'")

    def report(self):
        if self.missing:
            print(f"[docs] {self.code}: {len(self.missing)} fixed phrase(s) left in English:", file=sys.stderr)
            for text in sorted(self.missing):
                print(f"  {text}", file=sys.stderr)
        if self.untranslated:
            print(f"[docs] {self.code}: {len(self.untranslated)} component text(s) not in any table, shown in English "
                  "(tools/ci/check_translations.py finds them for the editor too):", file=sys.stderr)
            for package, text in sorted(self.untranslated):
                print(f"  {package}: {text[:100]}", file=sys.stderr)


# ----------------------------------------------------------------- header

# The site bar's words, as dekiengine.com's own menu has them.
BAR = {
    "ja": {"site": "サイト", "packages": "パッケージ", "blog": "ブログ", "docs": "ドキュメント", "donate": "寄付",
           "brief": "組み込みディスプレイとデスクトップのためのモジュール式 C++ 2D エンジン",
           "package brief": "Deki パッケージリファレンス", "language": "言語"},
    "zh": {"site": "网站", "packages": "包", "blog": "博客", "docs": "文档", "donate": "捐赠",
           "brief": "面向嵌入式显示屏和桌面的模块化 C++ 2D 引擎",
           "package brief": "Deki 包参考", "language": "语言"},
    "ko": {"site": "사이트", "packages": "패키지", "blog": "블로그", "docs": "문서", "donate": "후원",
           "brief": "임베디드 디스플레이와 데스크톱을 위한 모듈식 C++ 2D 엔진",
           "package brief": "Deki 패키지 레퍼런스", "language": "언어"},
}

# Pages that exist in every language. Anything else (the API reference) is
# English only, so the menu sends it to the other language's front page.
TRANSLATED_PAGES = r"^(index|pages|engine-core|settings|guide-[a-z0-9-]+)\.html$"


def bar_word(code, key, english):
    return BAR.get(code, {}).get(key, english)


def header(template, code):
    """theme/header.html for one language: the site bar in that language and
    pointing at that language's site, its Noto font, and the language menu."""
    out = template
    if code != "en":
        site = f"https://dekiengine.com/{code}/"
        for path in ("packages/", "blog/", "donate/"):
            out = out.replace(f'href="https://dekiengine.com/{path}"', f'href="{site}{path}"')
        out = out.replace('href="https://dekiengine.com/discord"', f'href="{site}discord/"')
        out = out.replace('href="https://dekiengine.com/"', f'href="{site}"')
        for key in ("site", "packages", "blog"):
            out = out.replace(f'<span class="deki-nav-label">{key}</span>', f'<span class="deki-nav-label">{bar_word(code, key, key)}</span>')
        out = out.replace('<span class="deki-bar-tag">docs</span>', f'<span class="deki-bar-tag">{bar_word(code, "docs", "docs")}</span>')
        out = out.replace("<span>donate</span>", f"<span>{bar_word(code, 'donate', 'donate')}</span>")
        out = out.replace("<!--DEKI_FONTS-->", f'<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family={NOTO[code]}:wght@400;500;700&display=swap">')
    else:
        out = out.replace("<!--DEKI_FONTS-->\n", "")

    links = []
    for other in LABELS:
        if code == "en":
            root = "" if other == "en" else f"{other}/"
        else:
            root = "../" if other == "en" else f"../{other}/"
        current = ' aria-current="true"' if other == code else ""
        links.append(f'<a href="$relpath^{root}index.html" data-deki-root="$relpath^{root}" hreflang="{other}" '
                     f'lang="{other}"{current}>{LABELS[other]}</a>')
    menu = (f'<details class="deki-lang"><summary aria-label="{bar_word(code, "language", "Language")}: {LABELS[code]}">'
            f'<span class="deki-nav-label">{LABELS[code]}</span></summary><div class="deki-lang-menu">'
            + "".join(links) + "</div></details>\n"
            "    <script>(function () {\n"
            "      var page = location.pathname.split('/').pop() || 'index.html';\n"
            f"      if (!/{TRANSLATED_PAGES}/.test(page)) return;\n"
            "      document.querySelectorAll('[data-deki-root]').forEach(function (a) {\n"
            "        a.href = a.getAttribute('data-deki-root') + page + location.hash;\n"
            "      });\n"
            "    })();</script>")
    return out.replace("<!--DEKI_LANGUAGES-->", menu)


# ----------------------------------------------------------------- guides

FENCE = re.compile(r"^\s*(```|~~~)")
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*(\{#[^}]*\})?\s*$")
LINK = re.compile(r"(!?)\[([^\]]*)\]\(([^)\s]+)\)")


def slug(heading):
    """GitHub's anchor for a heading, which is what the guides link to."""
    text = re.sub(r"`", "", heading).strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def headings(text):
    """(level, text) of every heading outside a code block."""
    out, fenced = [], False
    for line in text.splitlines():
        if FENCE.match(line):
            fenced = not fenced
            continue
        if not fenced:
            m = HEADING.match(line)
            if m:
                out.append((len(m.group(1)), m.group(2)))
    return out


def guide_title(text):
    found = headings(text)
    return found[0][1] if found else ""


def guide_page(text, name, guides, english_text=None, depth=0):
    """One guide as a Doxygen page.

    The first heading becomes the page, labelled guide-<name>; every other
    heading gets a label made from the English heading's GitHub anchor, so a
    link like `platforms.md#where-the-assets-go` works in every language.
    Links to other guides go to their pages; links to other files in the
    repository go to GitHub. `depth` is how many folders below docs/ the file
    sits (1 for a translation).
    """
    english = headings(english_text if english_text is not None else text)
    own = headings(text)
    labels = None
    if len(english) == len(own) and all(a[0] == b[0] for a, b in zip(english, own)):
        labels = [slug(h[1]) for h in english]
    elif english_text is not None:
        print(f"[docs] guide {name}: headings differ from the English, sections are not linkable", file=sys.stderr)

    out, fenced, index = [], False, 0
    for line in text.splitlines():
        if FENCE.match(line):
            fenced = not fenced
            out.append(line)
            continue
        if fenced:
            out.append(line)
            continue
        m = HEADING.match(line)
        if m:
            if index == 0:
                line = f"# {m.group(2)} {{#guide-{name}}}"
            elif labels:
                line = f"{m.group(1)} {m.group(2)} {{#guide-{name}-{labels[index]}}}"
            index += 1
        line = LINK.sub(lambda lm: _link(lm, name, guides, depth), line)
        out.append(_emphasis(line) if depth else line)
    return "\n".join(out) + "\n"


BOLD = re.compile(r"\*\*(?=\S)(.+?)(?<=\S)\*\*")
ITALIC = re.compile(r"(?<![\w*\\])\*(?=[^\s*])([^*]+?)(?<=\S)\*(?![\w*])")


def _emphasis(line):
    """**bold** and *italic* as HTML outside code spans.

    Doxygen only takes `**` as emphasis next to a space or ASCII punctuation,
    and Japanese, Chinese and Korean put neither around a word: 「。**SD
    カード**パネル」 came out with its asterisks showing.
    """
    spans = []

    def keep(m):
        spans.append(m.group(0))
        return f"\x00{len(spans) - 1}\x00"

    text = re.sub(r"`+[^`]*`+", keep, line)
    text = ITALIC.sub(r"<em>\1</em>", BOLD.sub(r"<b>\1</b>", text))
    return re.sub(r"\x00(\d+)\x00", lambda m: spans[int(m.group(1))], text)


def _link(m, name, guides, depth):
    bang, label, target = m.groups()
    if re.match(r"^[a-z][a-z0-9+.-]*:", target) or bang:
        return m.group(0)
    path, _, anchor = target.partition("#")
    if not path:
        return f"[{label}](#guide-{name}-{anchor})"
    # A guide's translation sits beside the other guides' translations.
    sibling = posixpath.normpath(path)
    if "/" not in sibling and sibling.endswith(".md") and sibling[:-len(".md")] in guides:
        other = sibling[:-len(".md")]
        return f"[{label}](guide-{other}.html" + (f"#guide-{other}-{anchor}" if anchor else "") + ")"
    resolved = posixpath.normpath(posixpath.join("docs", *(["lang"] * depth), path))
    if resolved.startswith("deki-engine/"):
        url = ENGINE_REPO + resolved[len("deki-engine/"):]
    else:
        url = EDITOR_REPO + resolved
    return f"[{label}]({url}" + (f"#{anchor}" if anchor else "") + ")"
