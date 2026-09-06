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
        self.assertIn("Part of [Deki](https://dekiengine.github.io/docs/)", page)
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
