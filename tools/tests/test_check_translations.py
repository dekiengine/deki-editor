"""Tests for tools/ci/check_translations.py.

Every component category and description went unchecked because the codegen
writes a comment after each field's comma, which then led the next field.
"""
import importlib.util
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("check_translations", REPO / "tools" / "ci" / "check_translations.py")
check_translations = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(check_translations)

GENERATED = '''const ::Deki::ComponentMeta DekiGen_Deki2D_SpriteComponent_Meta = {
    "SpriteComponent",
    ::Deki::TypeName<Deki2D::SpriteComponent>(),  // serializedName: the scene file's "type"
    "2D",  // DEKI_CATEGORY
    "Draws a sprite, see https://example.com // not a comment.",  // DEKI_DESCRIPTION
    ::Deki::TypeId<Deki2D::SpriteComponent>(),
    ::Deki::TypeId<DekiRendering::RendererComponent>(),
    false,   // isAbstract
    false,   // isUnique
    false,   // bootSceneOnly
    false,   // platformSceneOnly
    sizeof(Deki2D::SpriteComponent),
    DekiGen_Deki2D_SpriteComponent_TypeInfo.create,
    DekiGen_Deki2D_SpriteComponent_Props, 7,
    {}, false,
    "Sprite Renderer",   // displayNameOverride (DEKI_DISPLAY_NAME)
    "Sprites",   // settingsSection
    &DekiGen_Deki2D_SpriteComponent_TypeInfo,
};
'''


class ReflectionTexts(unittest.TestCase):
    def test_category_and_description_are_read_past_the_comments(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "Deki2DReflection0.gen.cpp"
            path.write_text(GENERATED, encoding="utf-8")
            found = check_translations.reflection_texts([path])
        self.assertIn("Sprite Component", found)
        self.assertIn("2D", found)
        self.assertIn("Draws a sprite, see https://example.com // not a comment.", found)
        self.assertIn("Sprite Renderer", found)
        self.assertIn("Sprites", found)

    def test_comments_inside_strings_stay(self):
        text = '"a // b", // gone\n"c\\" // d" // gone too'
        self.assertEqual(check_translations.strip_line_comments(text), '"a // b", \n"c\\" // d" ')


if __name__ == "__main__":
    unittest.main()
