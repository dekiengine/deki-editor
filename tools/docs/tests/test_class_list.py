import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import class_list  # noqa: E402

ROW = ('<tr id="row_{id}" class="even"><td class="entry"><span class="spacer">&#160;</span>'
       '<span class="icona"><span class="icon">{icon}</span></span>'
       '<a class="el" href="{href}" target="_self">{name}</a></td><td class="desc">{desc}</td></tr>\n')


def page(*rows):
    body = "".join(ROW.format(id=i, icon=k, href=h, name=n, desc=d) for i, k, h, n, d in rows)
    return f'<div class="contents"><div class="directory">\n<table class="directory">\n{body}</table>\n</div></div>'


SAMPLE = page(("0_", "N", "ns_deki.html", "Deki", "not shown"),
              ("0_0_", "N", "ns_fmt.html", "SceneFormat", ""),
              ("0_0_0_", "C", "fmt_parser.html", "Parser", "Reads <b>scenes</b>"),
              ("0_1_", "C", "scene.html", "Scene", "One screen"),
              ("0_2_", "C", "object.html", "Object", "A thing"),
              ("1_", "N", "ns_editor.html", "DekiEditor", ""),
              ("1_0_", "C", "win.html", "EditorWindow", "A window"))


class ClassListTests(unittest.TestCase):
    def test_lists_classes_and_not_namespaces(self):
        names = [c[1] for c in class_list.classes(SAMPLE)]
        self.assertEqual(names, ["SceneFormat::Parser", "Scene", "Object", "EditorWindow"])

    def test_groups_sort_and_plain_descriptions(self):
        out = class_list.rewrite(SAMPLE)
        self.assertNotIn("<table", out)
        self.assertLess(out.index(">Engine<"), out.index(">Editor<"))
        engine = out[out.index(">Engine<"):out.index(">Editor<")]
        self.assertLess(engine.index(">Object<"), engine.index(">Scene<"))
        self.assertIn(">Reads scenes<", out)

    def test_start_here_comes_first(self):
        out = class_list.rewrite(SAMPLE)
        self.assertLess(out.index(">Start here<"), out.index(">Engine<"))
        start = out[out.index(">Start here<"):out.index(">Engine<")]
        self.assertLess(start.index(">Object<"), start.index(">Scene<"))
        self.assertEqual(class_list.START, sorted(class_list.START))

    def test_changed_markup_stops_the_build(self):
        with self.assertRaises(SystemExit):
            class_list.rewrite("<div>nothing here</div>")


if __name__ == "__main__":
    unittest.main()
