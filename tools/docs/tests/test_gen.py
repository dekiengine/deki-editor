"""Unit tests for gen_component_docs.py: run with

    python -m unittest discover -s tools/docs/tests

from the deki-editor root (or any directory; the module is located relative
to this file).
"""

import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import gen_component_docs as gen  # noqa: E402

FIXTURE = {
    "schemaVersion": 1,
    "engineVersion": "1.3.0",
    "packages": [
        {
            "id": "deki-2d",
            "name": "Deki 2D",
            "components": [
                {
                    "name": "SpriteComponent",
                    "displayName": "Sprite",
                    "guid": "ca17ba76-f46d-484d-97ff-6e8e5497ef8d",
                    "category": "2D",
                    "description": "Draws a sprite.",
                    "base": "RendererComponent",
                    "isAbstract": False,
                    "isUnique": False,
                    "bootSceneOnly": False,
                    "platformSceneOnly": False,
                    "properties": [
                        {"name": "sortingOrder", "type": "int32", "default": 0,
                         "inheritedFrom": "RendererComponent"},
                        {"name": "tintColor", "displayName": "Tint", "type": "Color",
                         "tooltip": "Multiplied with every pixel", "default": 4294967295},
                        {"name": "renderMode", "type": "enum", "enumValues": ["Normal", "Additive"],
                         "default": 1},
                        {"name": "width", "type": "float", "unit": "m",
                         "range": {"min": 0.0, "max": 100.0, "slider": True}, "default": 0.0},
                        {"name": "sprite", "type": "AssetRef", "assetType": "Texture", "default": ""},
                    ],
                }
            ],
            "settings": [],
        },
        {
            "id": "deki-rendering",
            "name": "Deki Rendering",
            "components": [],
            "settings": [
                {
                    "section": "Rendering",
                    "name": "RenderingProjectSettings",
                    "guid": "x",
                    "category": "Settings",
                    "description": "",
                    "properties": [
                        {"name": "dirtyTileTracking", "type": "bool", "default": False},
                    ],
                }
            ],
        },
    ],
    "engine": {
        "components": [
            {"name": "SetupComponent", "displayName": "Setup", "guid": "y", "category": "Core",
             "description": "", "isAbstract": False, "isUnique": True, "bootSceneOnly": True,
             "platformSceneOnly": False, "properties": []}
        ],
        "settings": [
            {"section": "Core", "name": "CoreProjectSettings", "guid": "z", "category": "Settings",
             "description": "", "properties": [{"name": "targetFps", "type": "int32", "default": 60}]}
        ],
    },
}

REGISTRY = {
    "schemaVersion": 1,
    "packages": [
        {"id": "deki-2d", "displayName": "2D", "description": "Sprites and text", "repo": "dekiengine/deki-2d",
         "latest": "1.2.0", "requires": ["deki-rendering"], "docs": "https://dekiengine.github.io/deki-2d/"},
        {"id": "deki-rendering", "displayName": "Rendering", "description": "Rasterizer",
         "repo": "dekiengine/deki-rendering", "latest": "1.2.0", "requires": []},
    ],
}


class GenerateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.meta = os.path.join(self.tmp.name, "metadata.json")
        self.registry = os.path.join(self.tmp.name, "index.json")
        with open(self.meta, "w", encoding="utf-8") as f:
            json.dump(FIXTURE, f)
        with open(self.registry, "w", encoding="utf-8") as f:
            json.dump(REGISTRY, f)
        self.out = os.path.join(self.tmp.name, "md")
        self.pkgs = os.path.join(self.tmp.name, "md-packages")
        gen.generate(self.meta, self.out, registry_path=self.registry, packages_out=self.pkgs,
                     main_url="https://dekiengine.github.io/docs/")

    def tearDown(self):
        self.tmp.cleanup()

    def read(self, *rel):
        with open(os.path.join(*rel), encoding="utf-8") as f:
            return f.read()

    def test_main_index_links_each_package_to_its_own_site(self):
        index = self.read(self.out, "index.md")
        self.assertIn("# Deki {#mainpage}", index)
        self.assertIn("| [deki-2d](https://dekiengine.github.io/deki-2d/) | 1 | Sprites and text |", index)
        # no docs field in the registry -> derived from the repository
        self.assertIn("| [deki-rendering](https://dekiengine.github.io/deki-rendering/) | 0 | Rasterizer |", index)
        self.assertIn("[Engine core](engine-core.html)", index)
        self.assertNotIn("SpriteComponent", index)

    def test_main_site_has_no_package_pages(self):
        self.assertEqual(sorted(os.listdir(self.out)), ["engine.md", "index.md", "settings.md"])

    def test_engine_page(self):
        engine = self.read(self.out, "engine.md")
        self.assertIn("### SetupComponent {#engine-setupcomponent}", engine)
        self.assertIn("one per object, boot scene only", engine)
        self.assertIn("`targetFps`", engine)
        # settings types are components too, but they belong under Project settings only
        self.assertEqual(engine.count("CoreProjectSettings"), 1)
        self.assertNotIn("[CoreProjectSettings](#engine-coreprojectsettings)", engine)

    def test_escaping_protects_doxygen_from_prose(self):
        self.assertEqual(gen.esc("px<->m and #tag @x *y*"), "px&lt;-&gt;m and \\#tag \\@x \\*y\\*")

    def test_settings_page_links_package_sections_out(self):
        settings = self.read(self.out, "settings.md")
        self.assertIn("| Core | [engine](engine-core.html) | 1 |", settings)
        self.assertIn("| Rendering | [deki-rendering](https://dekiengine.github.io/deki-rendering/) | 1 |", settings)
        self.assertNotIn("`dirtyTileTracking`", settings)  # documented on the package's site

    def test_package_site_page(self):
        page = self.read(self.pkgs, "deki-2d", "index.md")
        self.assertIn("# deki-2d {#mainpage}", page)
        self.assertIn("Requires: [deki-rendering](https://dekiengine.github.io/deki-rendering/)", page)
        self.assertIn("[Deki docs](https://dekiengine.github.io/docs/)", page)
        self.assertIn("[SpriteComponent](#c-spritecomponent)", page)
        self.assertIn("### SpriteComponent {#c-spritecomponent}", page)
        self.assertIn("Inspector name: Sprite", page)
        self.assertIn("| `tintColor` | Color | `0xFFFFFFFF` |  | Multiplied with every pixel |", page)
        self.assertIn("| `renderMode` | enum | `Additive` | `Normal`, `Additive` |  |", page)
        self.assertIn("| `width` | float | `0` | 0 to 100 (slider); in m |  |", page)
        self.assertIn("AssetRef (Texture)", page)
        # inherited properties are one line linking to the base class, not repeated rows
        self.assertNotIn("| `sortingOrder` |", page)
        self.assertIn("Inherits `sortingOrder` from RendererComponent.", page)

        rendering = self.read(self.pkgs, "deki-rendering", "index.md")
        self.assertIn("## Project settings", rendering)
        self.assertIn("`dirtyTileTracking`", rendering)

    def test_docs_url_for_repo(self):
        self.assertEqual(gen.docs_url_for_repo("dekiengine/deki-2d"), "https://dekiengine.github.io/deki-2d/")
        self.assertEqual(gen.docs_url_for_repo("someone/pkg"), "https://someone.github.io/pkg/")

    def test_table_cells_escape_pipes(self):
        self.assertEqual(gen.esc("a|b"), "a\\|b")
        self.assertEqual(gen.esc(None), "")


if __name__ == "__main__":
    unittest.main()


class EmptyTableCells(unittest.TestCase):
    """An empty value must never reach Markdown as a bare pair of backticks.

    Doxygen reads `` as a code span that never closes and swallows every row
    after it into one cell, so the page shows the raw </td> and \ilinebr of
    the rest of the table. deki-sdl3-integration's features have no
    buildDefine -- they are stripped by their component types -- and that one
    empty cell destroyed its whole feature-flag table on the live docs site.
    """

    def test_empty_values_render_as_a_dash_not_empty_backticks(self):
        for value in ("", "   ", "\t", None):
            with self.subTest(value=value):
                got = gen.code(value)
                self.assertNotIn("``", got)
                self.assertEqual(got, gen.EMPTY_CELL)

    def test_real_values_still_render_as_code(self):
        self.assertEqual(gen.code("DEKI_FEATURE_SPRITE"), "`DEKI_FEATURE_SPRITE`")
        self.assertEqual(gen.code(0), "`0`")
        self.assertEqual(gen.code(False), "`False`")

    def test_a_backtick_in_a_value_cannot_close_the_span_early(self):
        self.assertEqual(gen.code("a`b"), "`a'b`")

    def test_a_feature_without_a_define_does_not_break_its_table(self):
        features = [
            {"id": "display", "displayName": "Display", "defaultEnabled": True},
            {"id": "sprite", "displayName": "Sprites", "buildDefine": "DEKI_FEATURE_SPRITE",
             "defaultEnabled": True},
        ]
        rows = [f"| {gen.esc(f.get('displayName'))} | {gen.code(f.get('buildDefine', ''))} | "
                f"{'on' if f.get('defaultEnabled', True) else 'off'} |" for f in features]
        for row in rows:
            self.assertNotIn("``", row)
            self.assertEqual(row.count("|"), 4, f"row is not three cells: {row}")


class PropertyTableColumns(unittest.TestCase):
    """Columns nobody filled in do not get to take width from the ones they did.

    Description was entirely empty in 24 of the 33 property tables across the
    package docs, Constraints in 5. Rendering them anyway cost the three
    columns a reader actually came for.
    """

    @staticmethod
    def props(**overrides):
        base = {"name": "width", "type": "float", "default": 0}
        base.update(overrides)
        return [base]

    def test_empty_columns_are_dropped(self):
        table = gen.property_table(self.props())
        self.assertIn("| Property | Type | Default |", table[0])
        self.assertNotIn("Constraints", table[0])
        self.assertNotIn("Description", table[0])

    def test_a_column_with_any_content_is_kept(self):
        table = gen.property_table(self.props(tooltip="How wide it is"))
        self.assertIn("Description", table[0])
        self.assertNotIn("Constraints", table[0])

    def test_every_row_has_the_same_cell_count_as_the_header(self):
        rows = [
            {"name": "a", "type": "float", "default": 0},
            {"name": "b", "type": "float", "default": 1, "tooltip": "only this one"},
        ]
        table = gen.property_table(rows)
        want = table[0].count("|")
        for line in table[:-1]:
            self.assertEqual(line.count("|"), want, f"ragged row: {line}")

    def test_the_separator_matches_the_header_width(self):
        table = gen.property_table(self.props(tooltip="x", sliderMin=0, sliderMax=10))
        self.assertEqual(table[0].count("|"), table[1].count("|"))
