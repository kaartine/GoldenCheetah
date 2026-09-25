#!/usr/bin/env python3
"""Rules of the Qt 6.8.3 WebEngine Memcheck exception generator, on synthetic reports."""

import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest
from xml.sax.saxutils import escape

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


class StructurePlacementTest(unittest.TestCase):
    """cycle_members.py places a chain by Valgrind's block_list of the loss records."""

    LISTING = """sending command leak_check full kinds definite,indirect any to pid 8
==8== 3,240 (200 direct, 3,040 indirect) bytes in 1 blocks are definitely lost in loss record 9 of 9
==8==    at 0x1: operator new(unsigned long) (vg_replace_malloc.c:501)
==8==    by 0x2: router() (router.cc:1)
==8==    by 0x3: main (control.cpp:9)
sending command block_list 1..8 to pid 8
==8== 3,400 (200 direct, 3,200 indirect) bytes in 1 blocks are definitely lost in loss record 8 of 9
==8==    at 0x1: operator new(unsigned long) (vg_replace_malloc.c:501)
==8==    by 0x4: ukm() (ukm.cc:1)
==8==    by 0x5: start_thread (in /usr/lib/libc.so.6)
==8== 0x10[200]
==8==   0x20[976] indirect loss record 7
==8==     0x30[8] indirect loss record 5
==8== 976 bytes in 1 blocks are indirectly lost in loss record 7 of 9
==8==    at 0x1: operator new(unsigned long) (vg_replace_malloc.c:501)
==8==    by 0x6: multiplex() (router.cc:2)
==8==    by 0x3: main (control.cpp:9)
==8== 8 bytes in 1 blocks are indirectly lost in loss record 5 of 9
==8==    at 0x1: operator new(unsigned long) (vg_replace_malloc.c:501)
==8==    by 0x7: shared() (x.cc:1)
==8==    by 0x3: main (control.cpp:9)
==8== 8 bytes in 1 blocks are indirectly lost in loss record 6 of 9
==8==    at 0x1: operator new(unsigned long) (vg_replace_malloc.c:501)
==8==    by 0x7: shared() (x.cc:1)
==8==    by 0x3: main (control.cpp:9)
sending command block_list 9 to pid 8
==8== 3,240 (200 direct, 3,040 indirect) bytes in 1 blocks are definitely lost in loss record 9 of 9
==8==    at 0x1: operator new(unsigned long) (vg_replace_malloc.c:501)
==8==    by 0x2: router() (router.cc:1)
==8==    by 0x3: main (control.cpp:9)
==8== 0x40[200]
==8==   0x50[8] indirect loss record 6
"""

    def test_members_are_placed_under_the_root_that_holds_them(self):
        members = load("cycle_members")
        with tempfile.TemporaryDirectory(prefix="gc-memcheck-tools-") as directory:
            path = Path(directory) / "block_list.txt"
            path.write_text(self.LISTING)
            new = "operator new(unsigned long)"
            placed = members.structures(str(path), {(new, "router()"), (new, "multiplex()"),
                                                    (new, "ukm()", "start_thread"), (new, "shared()")})
        self.assertEqual(placed[(new, "router()")], (3240, "definitely", 200))
        self.assertEqual(placed[(new, "multiplex()")], (3400, "indirectly", 976))
        self.assertEqual(placed[(new, "ukm()", "start_thread")], (3400, "definitely", 200))
        # the same chain in both structures (records 5 and 6) is not placed
        self.assertNotIn((new, "shared()"), placed)


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

    def test_structure_members_are_widened_and_bounded_by_their_own_structure(self):
        guard = load("check_widened")
        members, roots = FINAL.CYCLE["members"], FINAL.CYCLE["roots"]
        self.assertEqual({m["structure"] for m in members}, {3240, 3400})
        for structure in (3240, 3400):
            used = sum(m["size"] for m in members + roots if m["structure"] == structure)
            self.assertLessEqual(used, structure)
        for member in members:
            with self.subTest(structure=member["structure"], size=member["size"]):
                name, body = FINAL.widen(("Memcheck:Leak", "match-leak-kinds: indirect", *member["frames"]))
                self.assertTrue(name.startswith(f"ipcz-{member['structure']}-"))
                self.assertEqual(body[1], "match-leak-kinds: definite,indirect")
                label, indirect, totals = guard.rule_for({"frames": member["frames"], "demangled": []})
                self.assertEqual((indirect, totals), ({member["size"]}, {member["structure"]}))
        for root in roots:  # guarded on its full chain, never widened
            self.assertIsNone(FINAL.widen(("Memcheck:Leak", "match-leak-kinds: definite", *root["frames"]))[0])
            self.assertEqual(guard.rule_for({"frames": root["frames"], "demangled": []})[1:],
                             ({root["size"]}, {root["structure"]}))
        self.assertEqual(guard.rule_for({"frames": ["fun:_Znwm", "fun:other"], "demangled": []})[0], "no rule")

    def guard_verdict(self, records):
        """check_widened on the installed exception file and a report of (kind, bytes, frames)."""
        guard = load("check_widened")
        errors = []
        for kind, size, frames in records:
            stack = "".join(f"<frame><fn>{escape(f[4:])}</fn><obj>{LIB}</obj></frame>" for f in frames)
            sframes = "".join(f"<sframe><fun>{escape(f[4:])}</fun></sframe>" for f in frames)
            errors.append(f"<error><kind>{kind}</kind><xwhat><text>x</text><leakedbytes>{size}</leakedbytes>"
                          f"<leakedblocks>1</leakedblocks></xwhat><stack>{stack}</stack><suppression>"
                          f"<sname>x</sname><skind>Memcheck:Leak</skind>{sframes}</suppression></error>")
        with tempfile.TemporaryDirectory(prefix="gc-memcheck-tools-") as directory:
            path = Path(directory) / "memcheck.xml"
            path.write_text("<valgrindoutput>" + "".join(errors) + "</valgrindoutput>")
            supp = TOOLS.parent / "qt-6.8.3-webengine.supp"
            saved = sys.argv
            sys.argv = ["check_widened.py", str(supp), str(path)]
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    return guard.main()
            finally:
                sys.argv = saved

    def test_guard_rejects_a_structure_swap_and_a_grown_root(self):
        def named(structure):  # a member whose frames are all named functions
            return next(m for m in FINAL.CYCLE["members"] if m["structure"] == structure
                        and all(f.startswith("fun:") for f in m["frames"]))
        ukm = FINAL.CYCLE["roots"][0]
        for member, other in ((named(3240), 3400), (named(3400), 3240)):
            with self.subTest(structure=member["structure"]):
                own = ("Leak_DefinitelyLost", member["structure"], member["frames"])
                self.assertEqual(self.guard_verdict([own]), 0)
                self.assertEqual(self.guard_verdict([("Leak_DefinitelyLost", other, member["frames"])]), 1)
        self.assertEqual(self.guard_verdict([("Leak_DefinitelyLost", 3400, ukm["frames"])]), 0)
        self.assertEqual(self.guard_verdict([("Leak_DefinitelyLost", 3560, ukm["frames"])]), 1)
        self.assertEqual(self.guard_verdict([("Leak_IndirectlyLost", ukm["size"], ukm["frames"])]), 0)

    def test_glib_stanzas_by_object_or_function(self):
        self.assertTrue(FINAL.is_glib(("Memcheck:Leak", "obj:*/libglib-2.0.so.0*")))
        self.assertTrue(FINAL.is_glib(("Memcheck:Leak", "fun:g_main_context_dispatch")))
        self.assertFalse(FINAL.is_glib(("Memcheck:Leak", "fun:_ZN7QObject5eventEP6QEvent")))

    def test_only_the_listed_chains_are_widened(self):
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
