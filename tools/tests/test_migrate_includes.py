"""Tests for tools/migrate-includes.py, the public-header include rewrite."""

import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = os.path.join(os.path.dirname(__file__), "..", "migrate-includes.py")
spec = importlib.util.spec_from_file_location("migrate_includes", TOOL)
mi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mi)

TABLE = {
    "DekiObject.h": "deki/Object.h",
    "Scene.h": "deki/Scene.h",
    "reflection/DekiProperty.h": "deki/reflection/Property.h",
    "reflection/PropertyRef.h": "deki/reflection/PropertyRef.h",
    "providers/IDekiDisplay.h": "deki/providers/IDisplay.h",
}


class NewName(unittest.TestCase):
    def test_prefix_rule(self):
        self.assertEqual(mi.new_name("DekiObject.h"), "Object.h")
        self.assertEqual(mi.new_name("IDekiDisplay.h"), "IDisplay.h")
        self.assertEqual(mi.new_name("Scene.h"), "Scene.h")
        self.assertEqual(mi.new_name("ICamera.h"), "ICamera.h")
        self.assertEqual(mi.new_name("DesktopFileSystem.h"), "DesktopFileSystem.h")
        self.assertEqual(mi.new_name("DekiReflectionAbi.h"), "ReflectionAbi.h")


class PlanMoves(unittest.TestCase):
    def test_private_and_generated_stay(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp)
            for rel in ["DekiObject.h", "Scene.h", "LifecycleManager.h", "reflection/DekiProperty.h",
                        "providers/DekiProviderRegistry.h", "generated/X.gen.h", "DekiObject.cpp"]:
                p = src / rel
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text("", encoding="utf-8")
            moves = dict(mi.plan_moves(str(src)))
            self.assertEqual(moves, {
                "DekiObject.h": "deki/Object.h",
                "Scene.h": "deki/Scene.h",
                "reflection/DekiProperty.h": "deki/reflection/Property.h",
            })


class RewriteConsumer(unittest.TestCase):
    def test_exact_spellings_only(self):
        text = ('#include "DekiObject.h"\n'
                '#include <Scene.h>  // trailing\n'
                '#include "reflection/DekiProperty.h"\n'
                '#include "deki-rendering/Scene.h"\n'     # a package header, untouched
                '#include "RenderSystem.h"\n'
                '#include <deki/Object.h>\n')              # already migrated
        out, n = mi.rewrite_text(text, TABLE)
        self.assertEqual(n, 3)
        self.assertEqual(out, ('#include <deki/Object.h>\n'
                               '#include <deki/Scene.h>  // trailing\n'
                               '#include <deki/reflection/Property.h>\n'
                               '#include "deki-rendering/Scene.h"\n'
                               '#include "RenderSystem.h"\n'
                               '#include <deki/Object.h>\n'))

    def test_idempotent_and_crlf_preserved(self):
        text = '#include "DekiObject.h"\r\n'
        once, _ = mi.rewrite_text(text, TABLE)
        twice, n = mi.rewrite_text(once, TABLE)
        self.assertEqual(once, '#include <deki/Object.h>\r\n')
        self.assertEqual(twice, once)
        self.assertEqual(n, 0)

    def test_private_header_warns(self):
        warnings = []
        out, n = mi.rewrite_text('#include "LifecycleManager.h"\n', TABLE, warn=warnings.append)
        self.assertEqual(n, 0)
        self.assertEqual(len(warnings), 1)


class RewriteEngine(unittest.TestCase):
    def test_relative_and_sibling_includes_resolve(self):
        # A file that lived in src/reflection/ (now include/deki/reflection/).
        text = ('#include "PropertyRef.h"\n'          # sibling: reflection/PropertyRef.h
                '#include "../DekiObject.h"\n'        # parent: DekiObject.h
                '#include "../providers/IDekiDisplay.h"\n'
                '#include "generated/Foo.gen.h"\n'    # engine generated header
                '#include <vector>\n')
        out, n = mi.rewrite_text(text, TABLE, engine_dir="reflection")
        self.assertEqual(n, 4)
        self.assertEqual(out, ('#include <deki/reflection/PropertyRef.h>\n'
                               '#include <deki/Object.h>\n'
                               '#include <deki/providers/IDisplay.h>\n'
                               '#include "deki/generated/Foo.gen.h"\n'
                               '#include <vector>\n'))

    def test_engine_relative_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = tmp
            self.assertEqual(mi.engine_relative_dir(os.path.join(root, "include", "deki", "reflection", "X.h"), root), "reflection")
            self.assertEqual(mi.engine_relative_dir(os.path.join(root, "include", "deki", "X.h"), root), "")
            self.assertEqual(mi.engine_relative_dir(os.path.join(root, "src", "providers", "X.cpp"), root), "providers")
            self.assertIsNone(mi.engine_relative_dir(os.path.join(root, "tools", "gen.cpp"), root))


if __name__ == "__main__":
    unittest.main()
