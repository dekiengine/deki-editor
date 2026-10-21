"""Unit tests for localize.py and the language option of gen_component_docs.py:

    python -m unittest discover -s tools/docs/tests
"""

import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import gen_component_docs as gen  # noqa: E402
import localize  # noqa: E402

GUIDES = ["platforms", "screens", "memory"]

ENGLISH = """# Platforms

See [screens](screens.md) and [assets](#where-the-assets-go).

## Where the assets go

```bash
# not a heading
```

Checked by [the lint](../tools/ci/check_abi.py), done in [the engine](../deki-engine/src/x.cpp).
"""

JAPANESE = """# プラットフォーム

[画面](screens.md) と [アセット](#where-the-assets-go) を参照してください。

## アセットの置き場所 {#where-the-assets-go}

```bash
# not a heading
```

[リント](../../tools/ci/check_abi.py)。**SD カード**パネルと *斜体*、`a**b**c` はそのまま。
"""


class GuideTests(unittest.TestCase):
    def test_english_headings_get_page_and_section_labels(self):
        page = localize.guide_page(ENGLISH, "platforms", GUIDES)
        self.assertIn("# Platforms {#guide-platforms}", page)
        self.assertIn("## Where the assets go {#guide-platforms-where-the-assets-go}", page)

    def test_a_translation_takes_the_english_anchors(self):
        page = localize.guide_page(JAPANESE, "platforms", GUIDES, ENGLISH, depth=1)
        self.assertIn("## アセットの置き場所 {#guide-platforms-where-the-assets-go}", page)
        self.assertNotIn("{#where-the-assets-go}", page)

    def test_code_blocks_are_left_alone(self):
        page = localize.guide_page(ENGLISH, "platforms", GUIDES)
        self.assertIn("# not a heading\n", page)
        self.assertNotIn("not a heading {#", page)

    def test_links_between_guides_go_to_their_pages(self):
        for text, depth in ((ENGLISH, 0), (JAPANESE, 1)):
            page = localize.guide_page(text, "platforms", GUIDES, ENGLISH, depth)
            self.assertIn("](guide-screens.html)", page)
            self.assertIn("](#guide-platforms-where-the-assets-go)", page)

    def test_links_to_repository_files_go_to_github(self):
        for text, depth in ((ENGLISH, 0), (JAPANESE, 1)):
            page = localize.guide_page(text, "platforms", GUIDES, ENGLISH, depth)
            self.assertIn("](https://github.com/dekiengine/deki-editor/blob/master/tools/ci/check_abi.py)", page)
        page = localize.guide_page(ENGLISH, "platforms", GUIDES)
        self.assertIn("](https://github.com/dekiengine/deki-engine/blob/master/src/x.cpp)", page)

    def test_emphasis_becomes_html_in_a_translation(self):
        page = localize.guide_page(JAPANESE, "platforms", GUIDES, ENGLISH, depth=1)
        self.assertIn("<b>SD カード</b>パネル", page)
        self.assertIn("<em>斜体</em>", page)
        self.assertIn("`a**b**c`", page)

    def test_slug_matches_github(self):
        self.assertEqual(localize.slug("Origin: edit, update, restore"), "origin-edit-update-restore")
        self.assertEqual(localize.slug("The `Deki::Buffer` type"), "the-dekibuffer-type")


class HeaderTests(unittest.TestCase):
    TEMPLATE = ('<link href="x" rel="stylesheet">\n<!--DEKI_FONTS-->\n'
                '<a href="https://dekiengine.com/"><span class="deki-nav-label">site</span></a>'
                '<a href="https://dekiengine.com/blog/"><span class="deki-nav-label">blog</span></a>'
                '<span class="deki-bar-tag">docs</span><!--DEKI_LANGUAGES-->')

    def test_english_menu_points_into_the_language_folders(self):
        out = localize.header(self.TEMPLATE, "en")
        self.assertIn('data-deki-root="$relpath^ja/"', out)
        self.assertIn('data-deki-root="$relpath^"', out)
        self.assertNotIn("DEKI_", out)
        self.assertNotIn("Noto", out)

    def test_a_language_points_its_bar_at_its_own_site(self):
        out = localize.header(self.TEMPLATE, "ja")
        self.assertIn('href="https://dekiengine.com/ja/"', out)
        self.assertIn('href="https://dekiengine.com/ja/blog/"', out)
        self.assertIn(">ブログ<", out)
        self.assertIn("Noto+Sans+JP", out)
        self.assertIn('data-deki-root="$relpath^../"', out)
        self.assertIn('data-deki-root="$relpath^../ko/"', out)
        self.assertIn('lang="ja" aria-current="true"', out)


class LanguageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = self.tmp.name
        meta = {"engineVersion": "0.19.0", "engine": {"components": [], "settings": []}, "packages": [
            {"id": "deki-2d", "components": [
                {"name": "SpriteComponent", "displayName": "Sprite", "category": "2D",
                 "description": "Draws a sprite.",
                 "properties": [{"name": "tint", "type": "Color", "tooltip": "Multiplied in.", "default": 0}]}],
             "settings": []}]}
        self.meta = os.path.join(root, "metadata.json")
        with open(self.meta, "w", encoding="utf-8") as f:
            json.dump(meta, f)
        table = os.path.join(root, "packages", "deki-2d", "editor", "lang")
        os.makedirs(table)
        with open(os.path.join(table, "ja.json"), "w", encoding="utf-8") as f:
            json.dump({"Draws a sprite.": "スプライトを描画します。", "Multiplied in.": "掛け合わせます。",
                       "Sprite": "スプライト", "2D": "2D"}, f, ensure_ascii=False)
        guides = os.path.join(root, "docs")
        os.makedirs(os.path.join(guides, "ja"))
        for name, text in (("units", "# Units\n\nText.\n"), ("screens", "# Screens\n\nText.\n")):
            with open(os.path.join(guides, name + ".md"), "w", encoding="utf-8") as f:
                f.write(text)
        with open(os.path.join(guides, "ja", "units.md"), "w", encoding="utf-8") as f:
            f.write("# 単位\n\n本文。\n")
        self.out, self.pkgs = os.path.join(root, "md"), os.path.join(root, "md-packages")
        gen.generate(self.meta, self.out, os.path.join(root, "packages"), None, self.pkgs,
                     "https://dekiengine.github.io/docs/", "ja", None, None, guides)

    def tearDown(self):
        self.tmp.cleanup()
        gen.L = localize.Lang()

    def read(self, *rel):
        with open(os.path.join(*rel), encoding="utf-8") as f:
            return f.read()

    def test_component_text_comes_from_the_package_table(self):
        page = self.read(self.pkgs, "deki-2d", "index.md")
        self.assertIn("スプライトを描画します。", page)
        self.assertIn("掛け合わせます。", page)
        self.assertIn("インスペクターでの名前: スプライト", page)
        self.assertIn("`tint`", page)

    def test_links_stay_within_the_language(self):
        page = self.read(self.pkgs, "deki-2d", "index.md")
        self.assertIn("(https://dekiengine.github.io/docs/ja/)", page)
        index = self.read(self.out, "index.md")
        self.assertIn("(https://dekiengine.github.io/deki-2d/ja/)", index)
        self.assertIn("(https://dekiengine.github.io/docs/annotated.html)", index)

    def test_guides_are_listed_and_untranslated_ones_say_so(self):
        index = self.read(self.out, "index.md")
        self.assertIn("- [単位](guide-units.html)", index)
        self.assertIn("- [Screens](guide-screens.html)", index)
        self.assertIn("このガイドはまだ翻訳されていません。", self.read(self.out, "guide-screens.md"))
        self.assertNotIn("まだ翻訳", self.read(self.out, "guide-units.md"))

    def test_every_phrase_has_a_translation_in_every_language(self):
        english = set()
        for code in ("ja", "zh", "ko"):
            with open(os.path.join(os.path.dirname(HERE), "lang", f"{code}.json"), encoding="utf-8") as f:
                keys = set(json.load(f))
            english = english or keys
            self.assertEqual(keys, english, code)
        self.assertFalse(gen.L.missing, sorted(gen.L.missing))


if __name__ == "__main__":
    unittest.main()
