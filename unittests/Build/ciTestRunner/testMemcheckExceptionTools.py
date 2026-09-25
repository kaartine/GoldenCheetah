#!/usr/bin/env python3
"""Rules of the Qt 6.8.3 WebEngine Memcheck exception generator, on synthetic reports."""

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
TOOLS = Path(__file__).resolve().parents[3] / ".github/scripts/qt-6.8.3-webengine-control"


def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


GEN = load("gen_supp")
FINAL = load("final_supp")
LIB = "/opt/Qt/6.8.3/gcc_64/lib/libQt6WebEngineCore.so.6.8.3"
CORE = "/opt/Qt/6.8.3/gcc_64/lib/libQt6Core.so.6.8.3"
PROGRAM = "/control/build/webengine_control"


def report(errors):
    """Memcheck XML with (kind, skind, [(fn, obj), ...]) records and gen-suppressions blocks."""
    parts = ["<valgrindoutput>"]
    for kind, skind, frames in errors:
        stack = "".join(f"<frame><fn>{fn}</fn><obj>{obj}</obj></frame>" for fn, obj in frames)
        sframes = "".join(f"<sframe><fun>{fn}</fun></sframe>" if fn else f"<sframe><obj>{obj}</obj></sframe>"
                          for fn, obj in frames)
        parts.append(f"<error><kind>{kind}</kind><stack>{stack}</stack><suppression>"
                     f"<sname>x</sname><skind>{skind}</skind>{sframes}</suppression></error>")
    parts.append("</valgrindoutput>")
    return "".join(parts)


class GeneratorRulesTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gc-memcheck-tools-")
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def stanzas(self, errors):
        path = self.root / "control.xml"
        path.write_text(report(errors))
        return [(cut, stanza) for _, cut, stanza in GEN.stanzas(str(path))]

    def test_cut_classes_program_complete_truncated(self):
        named = [("_Znwm", "/vgpreload_memcheck.so")] + [(f"f{i}", LIB) for i in range(8)]
        cases = ((named + [("main", PROGRAM)], "program"),
                 (named + [("start_thread", "/libc.so.6"), ("clone", "/libc.so.6")], "complete"),
                 (named, "truncated"))  # neither the program nor a thread root: --num-callers
        for frames, expected in cases:
            with self.subTest(expected=expected):
                cuts = [cut for cut, _ in self.stanzas([("Leak_IndirectlyLost", "Memcheck:Leak", frames)])]
                self.assertEqual(cuts, [expected])

    def test_allocator_only_prefix_before_the_program_gives_no_stanza(self):
        self.assertEqual(self.stanzas([("Leak_DefinitelyLost", "Memcheck:Leak",
                                        [("malloc", "/vgpreload_memcheck.so"), ("_Znwm", "/libstdc++.so.6"),
                                         ("main", PROGRAM)])]), [])

    def test_library_cut_needs_seven_frames_in_the_allocating_library(self):
        def chain(depth):
            return ([("_Znwm", "/vgpreload_memcheck.so")] + [(f"w{i}", LIB) for i in range(depth)]
                    + [("QObject::event", CORE), ("main", PROGRAM)])
        cuts = [cut for cut, _ in self.stanzas([("Leak_IndirectlyLost", "Memcheck:Leak", chain(7))])]
        self.assertIn("library", cuts)
        cuts = [cut for cut, _ in self.stanzas([("Leak_IndirectlyLost", "Memcheck:Leak", chain(6))])]
        self.assertNotIn("library", cuts)


class ClassificationTest(unittest.TestCase):
    def body(self, skind, kinds, frames):
        return (skind, *([f"match-leak-kinds: {kinds}"] if kinds else []), *frames)

    def test_error_records_only_with_the_full_chain(self):
        frames = ("fun:__writev", "fun:writev", "obj:*/libxcb.so.1*", "fun:xcb_wait_for_reply")
        for cut, allowed in (("library", False), ("truncated", False), ("dispatcher", False),
                             ("program", True), ("complete", True)):
            with self.subTest(cut=cut):
                self.assertEqual(FINAL.classify(self.body("Memcheck:Param", None, frames), cut)["allowed"], allowed)

    def test_unnamed_library_cuts_only_for_indirect_leaks(self):
        frames = ("fun:_Znwm",) + ("obj:*/libQt6WebEngineCore.so.6*",) * 8
        self.assertTrue(FINAL.classify(self.body("Memcheck:Leak", "indirect", frames), "library")["allowed"])
        for kinds in ("definite", "possible"):
            with self.subTest(kinds=kinds):
                self.assertFalse(FINAL.classify(self.body("Memcheck:Leak", kinds, frames), "truncated")["allowed"])
        named = frames[:3] + ("fun:_ZN15QtWebEngineCore14ProfileAdapterD1Ev",)
        self.assertTrue(FINAL.classify(self.body("Memcheck:Leak", "definite", named), "library")["allowed"])
        generic = frames[:3] + ("fun:_ZN7QObject5eventEP6QEvent",)
        self.assertFalse(FINAL.classify(self.body("Memcheck:Leak", "definite", generic), "library")["allowed"])

    def test_glib_stanzas_by_object_or_function(self):
        self.assertTrue(FINAL.is_glib(("Memcheck:Leak", "obj:*/libglib-2.0.so.0*")))
        self.assertTrue(FINAL.is_glib(("Memcheck:Leak", "fun:g_main_context_dispatch")))
        self.assertFalse(FINAL.is_glib(("Memcheck:Leak", "fun:_ZN7QObject5eventEP6QEvent")))

    def test_only_the_two_listed_chains_are_widened(self):
        for label, chain in FINAL.WIDENED_CHAINS.items():
            with self.subTest(chain=label):
                name, body = FINAL.widen(("Memcheck:Leak", "match-leak-kinds: definite", *chain))
                self.assertEqual((name, body[1]), (label, "match-leak-kinds: definite,indirect"))
        other = ("Memcheck:Leak", "match-leak-kinds: definite", "fun:_Znwm", "fun:other")
        self.assertEqual(FINAL.widen(other), (None, other))
        possible = ("Memcheck:Leak", "match-leak-kinds: possible", *FINAL.WIDENED_CHAINS["fallback-surface"])
        self.assertEqual(FINAL.widen(possible)[0], None)


if __name__ == "__main__":
    unittest.main()
