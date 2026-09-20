"""Tests for tools/ci/check_allocations.py, the raw-allocation lint.

The rule it enforces is not stylistic: on the device a failed `new[]` reaches
__cxa_allocate_exception with exceptions off and aborts, which reboots the
board. The tests below pin the two ways a lint like this stops being useful -
false positives that get it switched off, and blind spots that let the thing
back in.
"""

import io
import os
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from tempfile import TemporaryDirectory

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "ci"))
import check_allocations as alloc  # noqa: E402


class CheckAllocations(unittest.TestCase):
    def run_check(self, files, extra=None):
        """Runs the lint over a throwaway packages tree. Returns (code, output)."""
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            for rel, body in files.items():
                path = root / rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(body, encoding="utf-8")
            buf = io.StringIO()
            with redirect_stdout(buf):
                code = alloc.main(["--packages-dir", str(root)] + (extra or []))
            return code, buf.getvalue()

    # --- what must be caught -------------------------------------------------

    def test_array_new_fails(self):
        code, out = self.run_check({"p/A.cpp": "void f() { auto* x = new uint8_t[n]; }\n"})
        self.assertEqual(code, 1)
        self.assertIn("new[]", out)
        self.assertIn("p/A.cpp:1", out)

    def test_array_new_of_a_qualified_type_fails(self):
        code, _ = self.run_check({"p/A.cpp": "auto* x = new Deki::Thing[count];\n"})
        self.assertEqual(code, 1)

    def test_malloc_family_fails(self):
        for call in ("malloc(n)", "std::malloc(n)", "calloc(n, 4)", "realloc(p, n)"):
            code, out = self.run_check({"p/A.cpp": f"void* p = {call};\n"})
            self.assertEqual(code, 1, call)
            self.assertIn("malloc/calloc/realloc", out)

    def test_heap_caps_fails(self):
        code, out = self.run_check(
            {"p/A.cpp": "void* p = heap_caps_malloc(n, MALLOC_CAP_DMA);\n"})
        self.assertEqual(code, 1)
        self.assertIn("heap_caps_*", out)

    def test_array_delete_fails(self):
        # A raw delete[] means the matching allocation is raw too, or that the
        # pointer came from Deki::Memory and delete[] is about to walk past a
        # header it does not know about.
        code, out = self.run_check({"p/A.cpp": "delete[] pixels;\n"})
        self.assertEqual(code, 1)
        self.assertIn("delete[]", out)

    def test_every_finding_is_reported_not_just_the_first(self):
        code, out = self.run_check({
            "p/A.cpp": "auto* a = new float[n];\n",
            "p/B.cpp": "void* b = malloc(n);\n",
        })
        self.assertEqual(code, 1)
        self.assertIn("p/A.cpp", out)
        self.assertIn("p/B.cpp", out)

    # --- what must not be caught --------------------------------------------

    def test_scalar_new_passes(self):
        # Out of scope on purpose: components and packages are created once at
        # load, and converting them is a separate job from the buffers.
        code, _ = self.run_check({"p/A.cpp": "auto* c = new MyComponent();\n"})
        self.assertEqual(code, 0)

    def test_indexing_a_new_object_is_not_an_array_new(self):
        code, _ = self.run_check({"p/A.cpp": "auto v = (new Thing())->items[0];\n"})
        self.assertEqual(code, 0)

    def test_deki_buffer_passes(self):
        code, _ = self.run_check({
            "p/A.cpp": 'Deki::Buffer<uint8_t> b;\n'
                       'b.Allocate(n, Deki::MemoryUse::Buffer, "ctx");\n'
        })
        self.assertEqual(code, 0)

    def test_a_mention_in_a_comment_passes(self):
        # This file's own header talks about `new float[]`; so does the header
        # of Buffer.h. A lint that trips on its own documentation gets turned
        # off within the week.
        code, _ = self.run_check({
            "p/A.cpp": "// the columns moved from new float[n] to Deki::Buffer\n"
                       "/* malloc(n) and heap_caps_malloc(n) are out too */\n"
        })
        self.assertEqual(code, 0)

    def test_a_mention_in_a_string_passes(self):
        code, _ = self.run_check({
            "p/A.cpp": 'DEKI_LOG_WARNING("do not call malloc here");\n'
        })
        self.assertEqual(code, 0)

    def test_host_side_directories_are_skipped(self):
        # editor/, tests/ and generated/ run on the host, where exceptions are
        # on and the heap is a desktop's.
        code, _ = self.run_check({
            "p/editor/A.cpp": "auto* x = new uint8_t[n];\n",
            "p/tests/B.cpp": "auto* x = new uint8_t[n];\n",
            "p/generated/C.cpp": "auto* x = new uint8_t[n];\n",
            "p/third_party/lib/D.cpp": "void* x = malloc(n);\n",
        })
        self.assertEqual(code, 0)

    def test_a_nested_editor_directory_is_skipped_too(self):
        code, _ = self.run_check({"p/editor/widgets/A.cpp": "auto* x = new uint8_t[n];\n"})
        self.assertEqual(code, 0)

    # --- waivers -------------------------------------------------------------

    def test_a_waiver_with_a_reason_is_honoured(self):
        code, out = self.run_check(
            {"p/A.cpp": "void* p = malloc(n);  // deki-alloc-ok: cJSON's allocator hook\n"},
            ["--verbose"])
        self.assertEqual(code, 0)
        self.assertIn("cJSON's allocator hook", out)

    def test_a_waiver_on_the_line_above_is_honoured(self):
        code, _ = self.run_check({
            "p/A.cpp": "// deki-alloc-ok: the C API frees this itself\n"
                       "void* p = malloc(n);\n"
        })
        self.assertEqual(code, 0)

    def test_a_waiver_without_a_reason_does_not_count(self):
        # Otherwise the escape hatch becomes a silent one, and nobody reviewing
        # a diff can tell which exceptions were argued for.
        code, _ = self.run_check({"p/A.cpp": "void* p = malloc(n);  // deki-alloc-ok:\n"})
        self.assertEqual(code, 1)

    def test_a_waiver_does_not_carry_to_the_next_line_but_one(self):
        code, _ = self.run_check({
            "p/A.cpp": "// deki-alloc-ok: only for the next line\n"
                       "int filler = 0;\n"
                       "void* p = malloc(n);\n"
        })
        self.assertEqual(code, 1)

    # --- operational ---------------------------------------------------------

    # --- the engine's own runtime ------------------------------------------

    def run_engine_check(self, files, extra=None):
        """Runs the lint over a throwaway repo holding an engine tree."""
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            for rel, body in files.items():
                path = root / rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(body, encoding="utf-8")
            buf = io.StringIO()
            with redirect_stdout(buf):
                code = alloc.main(["--packages-dir", str(root / "no-packages"),
                                   "--repo", str(root)] + (extra or []))
            return code, buf.getvalue()

    def test_the_engine_runtime_is_checked_too(self):
        # It was left out of the first version and immediately proved the
        # point: ProjectSettings kept its on-device settings chunk with a bare
        # realloc. An engine allocation reaches every project on every board.
        code, out = self.run_engine_check(
            {"deki-engine/src/Thing.cpp": "void* p = realloc(q, n);\n"})
        self.assertEqual(code, 1)
        self.assertIn("deki-engine/src/Thing.cpp:1", out)

    def test_engine_headers_are_checked_too(self):
        code, _ = self.run_engine_check(
            {"deki-engine/include/deki/Thing.h": "auto* x = new uint8_t[n];\n"})
        self.assertEqual(code, 1)

    def test_the_engines_host_only_platform_layers_are_skipped(self):
        # The editor and desktop platform layers run where exceptions are on.
        code, _ = self.run_engine_check({
            "deki-engine/src/platforms/editor/EditorDisplay.cpp":
                "m_Rgba = new uint8_t[w * h * 4];\ndelete[] m_Rgba;\n",
            "deki-engine/src/platforms/desktop/Thing.cpp": "void* p = malloc(n);\n",
        })
        self.assertEqual(code, 0)

    def test_the_esp32_platform_layer_is_not_skipped(self):
        # It runs on the device, so only its memory provider is exempt.
        code, _ = self.run_engine_check(
            {"deki-engine/src/platforms/esp32/Thing.cpp": "auto* x = new uint8_t[n];\n"})
        self.assertEqual(code, 1)

    def test_the_memory_subsystem_itself_is_exempt(self):
        # It is where the platform allocator is supposed to be called. Exempt
        # by path, which says "bottom of the stack" rather than "was granted
        # permission to break the rule".
        code, _ = self.run_engine_check({
            "deki-engine/src/providers/Memory.cpp":
                "void* p = malloc(n);  // tracking array, deliberately raw\n",
            "deki-engine/include/deki/providers/IMemoryProvider.h":
                "virtual void* AllocateInternal(size_t s) { return malloc(s); }\n",
            "deki-engine/src/platforms/esp32/ESP32MemoryProvider.cpp":
                "return heap_caps_malloc(size, MALLOC_CAP_DMA);\n",
        })
        self.assertEqual(code, 0)

    def test_the_engine_can_be_excluded_on_request(self):
        code, _ = self.run_engine_check(
            {"deki-engine/src/Thing.cpp": "void* p = malloc(n);\n"}, ["--no-engine"])
        self.assertEqual(code, 0)

    # --- operational ---------------------------------------------------------

    def test_nothing_to_check_passes_rather_than_blocking(self):
        # A checkout with neither the packages beside it nor an engine in it
        # cannot check anything, and should not block on that.
        buf = io.StringIO()
        with redirect_stdout(buf):
            code = alloc.main(["--packages-dir", "does/not/exist",
                               "--repo", "also/does/not/exist"])
        self.assertEqual(code, 0)
        self.assertIn("nothing to check", buf.getvalue())

    def test_a_missing_packages_directory_still_checks_the_engine(self):
        # The common case for a contributor who cloned only the editor: the
        # packages are absent, but the engine comes with it as a submodule and
        # is held to the same rule.
        code, out = self.run_engine_check(
            {"deki-engine/src/Thing.cpp": "auto* x = new float[n];\n"})
        self.assertEqual(code, 1)
        self.assertIn("deki-engine/src/Thing.cpp", out)

    def test_headers_are_scanned_too(self):
        # ParticlePool's columns lived in a header, which is where most of
        # this engine's buffers are declared and sized.
        code, _ = self.run_check({"p/A.h": "float* col = new float[cap];\n"})
        self.assertEqual(code, 1)

    def test_line_numbers_survive_stripped_comments(self):
        code, out = self.run_check({
            "p/A.cpp": "/* a block comment\n   spanning lines */\n"
                       "// then a line one\n"
                       "void* p = malloc(n);\n"
        })
        self.assertEqual(code, 1)
        self.assertIn("p/A.cpp:4", out)


if __name__ == "__main__":
    unittest.main()
