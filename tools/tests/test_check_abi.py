"""Tests for tools/ci/check_abi.py, the virtual-table lock.

Run:
    python -m unittest discover -s tools/tests
"""

import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "ci"))
import check_abi as abi  # noqa: E402

HEADER = r"""
#pragma once
// A comment mentioning virtual void NotReal();
/* another one: virtual int AlsoNotReal(); */
namespace Deki {

struct Helper { virtual void Nested() = 0; };

class DEKI_ENGINE_API Widget : public Base, private Other
{
public:
    virtual ~Widget() = default;
    virtual bool Initialize(int32_t width,
                            int32_t height = 240) = 0;
    virtual void Present(const uint8_t* fb, const char* tag = "x,y") const;
    virtual void Inline(int a) { doSomething(a); }
    void PlainMethod();
    void Overridden() override;   // reuses a base slot: not counted
    static void Static();
    struct Inner { virtual void Deeper(); };   // depth 1: not this class's vtable
    virtual std::unordered_map<uint32_t, uint32_t> Map(const std::vector<int>& v);
};

}  // namespace Deki
"""


class ExtractVirtuals(unittest.TestCase):
    def table(self, header=HEADER):
        text = abi.strip_comments(header)
        return abi.extract_virtuals(abi.class_body(text, "Widget"), "Widget")

    def test_declaration_order_and_normalisation(self):
        self.assertEqual(self.table(), [
            "~Widget()",
            "bool Initialize(int32_t width, int32_t height)",
            "void Present(const uint8_t* fb, const char* tag) const",
            "void Inline(int a)",
            "std::unordered_map<uint32_t, uint32_t> Map(const std::vector<int>& v)",
        ])

    def test_comments_nested_types_and_overrides_do_not_count(self):
        joined = "\n".join(self.table())
        for absent in ("NotReal", "AlsoNotReal", "Nested", "Deeper", "Overridden", "PlainMethod"):
            self.assertNotIn(absent, joined)

    def test_class_not_found_is_an_error(self):
        with self.assertRaises(abi.AbiError):
            abi.class_body(HEADER, "Missing")

    def test_preprocessor_conditional_inside_class_is_refused(self):
        header = HEADER.replace("    void PlainMethod();", "#ifdef X\n    virtual void Maybe();\n#endif")
        with self.assertRaises(abi.AbiError):
            self.table(header)


class LockRoundTrip(unittest.TestCase):
    def test_render_then_parse(self):
        tables = {"Deki::Widget": ["~Widget()", "bool Initialize(int32_t w)"], "Deki::Empty": []}
        version, parsed = abi.parse_lock(abi.render_lock(6, tables))
        self.assertEqual(version, 6)
        self.assertEqual(parsed, tables)

    def test_collect_reads_a_header_from_disk(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "Widget.h"
            p.write_text(HEADER, encoding="utf-8")
            got = abi.collect({"Deki::Widget": p})
            self.assertEqual(len(got["Deki::Widget"]), 5)

    def test_real_lock_matches_real_headers(self):
        # The check CI runs, from inside the test suite: the committed lock must
        # agree with the headers, or the commit that changed them forgot it.
        self.assertTrue(abi.LOCK.exists(), "tools/ci/abi.lock missing; run check_abi.py --update")
        version, locked = abi.parse_lock(abi.LOCK.read_text(encoding="utf-8"))
        self.assertEqual(version, abi.read_abi_version())
        self.assertEqual(locked, abi.collect())


if __name__ == "__main__":
    unittest.main()
