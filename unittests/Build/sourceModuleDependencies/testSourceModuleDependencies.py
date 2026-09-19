#!/usr/bin/env python3
"""Black-box and fixture tests for the source-module dependency gate."""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[3]
ANALYZER = REPOSITORY / "util" / "source_module_dependencies.py"
SPEC = importlib.util.spec_from_file_location("source_module_dependencies", ANALYZER)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load analyzer: {ANALYZER}")
ANALYZER_MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ANALYZER_MODULE)
MODULES = ANALYZER_MODULE.MODULES


class FixtureRepository:
    def __init__(self, root: Path) -> None:
        self.root = root
        for module in MODULES:
            (root / "src" / module).mkdir(parents=True)
        (root / "doc" / "design").mkdir(parents=True)

    def write(self, relative: str, contents: str | bytes) -> Path:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(contents, bytes):
            path.write_bytes(contents)
        else:
            path.write_text(contents, encoding="utf-8", newline="")
        return path

    def inventory(self) -> dict[str, object]:
        return ANALYZER_MODULE.build_inventory(self.root)


class SourceModuleDependenciesTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.fixture = FixtureRepository(Path(self.temporary.name))

    def run_analyzer(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(ANALYZER), "--repository", str(self.fixture.root), *arguments],
            check=False,
            encoding="utf-8",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def test_resolution_order_and_exact_cross_module_edges(self) -> None:
        self.fixture.write("src/Core/Common.h", "#pragma once\n")
        self.fixture.write("src/Cloud/Common.h", "#pragma once\n")
        self.fixture.write("src/Train/Unique.h", "#pragma once\n")
        self.fixture.write("src/Charts/Qualified.h", "#pragma once\n")
        self.fixture.write("src/ANT/Common.h", "#pragma once\n")
        self.fixture.write(
            "src/ANT/Consumer.cpp",
            '#include "Common.h"\n#include <Train/Unique.h>\n#include "Charts/Qualified.h"\n',
        )

        records = self.fixture.inventory()["cross_module_includes"]
        self.assertEqual(
            records,
            [
                {
                    "source": "src/ANT/Consumer.cpp",
                    "source_module": "ANT",
                    "target": "src/Charts/Qualified.h",
                    "target_module": "Charts",
                },
                {
                    "source": "src/ANT/Consumer.cpp",
                    "source_module": "ANT",
                    "target": "src/Train/Unique.h",
                    "target_module": "Train",
                },
            ],
        )

    def test_ambiguous_bare_header_fails_with_sorted_candidates(self) -> None:
        self.fixture.write("src/Core/Duplicate.h", "")
        self.fixture.write("src/Cloud/Duplicate.h", "")
        self.fixture.write("src/ANT/Consumer.cpp", '#include "Duplicate.h"\n')

        with self.assertRaisesRegex(
            ANALYZER_MODULE.DependencyError,
            r"candidates: src/Cloud/Duplicate\.h, src/Core/Duplicate\.h",
        ):
            self.fixture.inventory()

    def test_import_is_parsed_like_include(self) -> None:
        self.fixture.write("src/Core/Imported.h", "")
        self.fixture.write("src/Train/Consumer.mm", '#import "Core/Imported.h"\n')
        records = self.fixture.inventory()["cross_module_includes"]
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["target"], "src/Core/Imported.h")

    def test_wrong_case_first_party_include_fails_instead_of_looking_external(self) -> None:
        self.fixture.write("src/Core/Target.h", "")
        self.fixture.write("src/ANT/Consumer.cpp", "#include <target.h>\n")
        with self.assertRaisesRegex(
            ANALYZER_MODULE.DependencyError,
            r"first-party include has wrong case.*src/Core/Target\.h",
        ):
            self.fixture.inventory()

    def test_qualified_operand_preserves_subpath(self) -> None:
        self.fixture.write("src/Core/Target.h", "")
        self.fixture.write("src/Core/nested/Qualified.h", "")
        self.fixture.write(
            "src/ANT/Consumer.cpp",
            '#include "bogus/Target.h"\n#include "nested/Qualified.h"\n',
        )
        inventory = self.fixture.inventory()
        self.assertEqual(
            inventory["cross_module_includes"][0]["target"],
            "src/Core/nested/Qualified.h",
        )
        self.assertEqual(
            inventory["unresolved_quoted_includes"],
            [{"source": "src/ANT/Consumer.cpp", "include": "bogus/Target.h"}],
        )

    def test_common_cxx_and_inline_suffixes_are_scanned_case_insensitively(self) -> None:
        self.fixture.write("src/Core/Target.h", "")
        for suffix in ("cc", "cxx", "hh", "hxx", "m", "inc", "inl", "ipp", "tpp", "CXX"):
            self.fixture.write(
                f"src/ANT/Consumer_{suffix}.{suffix}", '#include "Target.h"\n'
            )
        records = self.fixture.inventory()["cross_module_includes"]
        self.assertEqual(len(records), 10)

    def test_comments_conditionals_generated_unresolved_and_macro_are_explicit(self) -> None:
        self.fixture.write(
            "src/ANT/Consumer.cpp",
            """// #include \"Ignored.h\"
/* #include \"AlsoIgnored.h\" */
#if FEATURE
#include \"RideDB_yacc.h\"
#include \"sipGenerated.h\"
#include \"MissingProjectHeader.h\"
#include GC_ANT_LIBUSB_HEADER
#endif
""",
        )

        inventory = self.fixture.inventory()
        self.assertEqual(
            inventory["generated_includes"],
            [
                {"source": "src/ANT/Consumer.cpp", "include": "RideDB_yacc.h", "generator": "parser"},
                {"source": "src/ANT/Consumer.cpp", "include": "sipGenerated.h", "generator": "sip"},
            ],
        )
        self.assertEqual(
            inventory["unresolved_quoted_includes"],
            [{"source": "src/ANT/Consumer.cpp", "include": "MissingProjectHeader.h"}],
        )
        self.assertEqual(
            inventory["macro_includes"],
            [{"source": "src/ANT/Consumer.cpp", "expression": "GC_ANT_LIBUSB_HEADER"}],
        )

    def test_generated_sip_products_are_excluded_but_authored_bridge_is_scanned(self) -> None:
        self.fixture.write("src/Core/CoreHeader.h", "")
        self.fixture.write("src/Python/SIP/sipGenerated.cpp", '#include "CoreHeader.h"\n')
        self.fixture.write("src/Python/SIP/Bindings.cpp", '#include "CoreHeader.h"\n')

        records = self.fixture.inventory()["cross_module_includes"]
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["source"], "src/Python/SIP/Bindings.cpp")
        self.assertEqual(records[0]["target"], "src/Core/CoreHeader.h")

    def test_output_is_deterministic_for_crlf_unicode_and_creation_order(self) -> None:
        first = self.fixture.write("src/Train/ÄäHeader.h", b"#pragma once\r\n")
        self.fixture.write("src/ANT/Z.cpp", '#include "ÄäHeader.h"\r\n')
        initial = ANALYZER_MODULE._canonical_json(self.fixture.inventory())
        first.unlink()
        self.fixture.write("src/Train/ÄäHeader.h", b"#pragma once\r\n")
        self.assertEqual(initial, ANALYZER_MODULE._canonical_json(self.fixture.inventory()))

    def test_src_root_build_products_do_not_affect_fixed_module_scope(self) -> None:
        initial = ANALYZER_MODULE._canonical_json(self.fixture.inventory())
        self.fixture.write("src/RideDB_yacc.cpp", '#include "DoesNotExist.h"\n')
        self.fixture.write("src/stable.h", b"\xff")
        self.assertEqual(initial, ANALYZER_MODULE._canonical_json(self.fixture.inventory()))

    def test_invalid_encoding_unsafe_path_and_symlink_fail_closed(self) -> None:
        bad = self.fixture.write("src/ANT/Bad.cpp", b"\xff")
        with self.assertRaisesRegex(ANALYZER_MODULE.DependencyError, "not valid UTF-8"):
            self.fixture.inventory()
        bad.write_text('#include "../../../../escape.h"\n', encoding="utf-8")
        with self.assertRaisesRegex(ANALYZER_MODULE.DependencyError, "escapes the repository"):
            self.fixture.inventory()
        bad.unlink()

        target = self.fixture.write("outside.h", "")
        link = self.fixture.root / "src" / "ANT" / "Linked.h"
        try:
            os.symlink(target, link)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"symlink creation is unavailable: {error}")
        with self.assertRaisesRegex(ANALYZER_MODULE.DependencyError, "symlink is not permitted"):
            self.fixture.inventory()

    def test_cli_write_check_and_stable_drift_diff(self) -> None:
        self.fixture.write("src/Core/Target.h", "")
        self.fixture.write("src/ANT/Consumer.cpp", '#include "Target.h"\n')
        baseline = "doc/design/source-module-dependencies.baseline"

        written = self.run_analyzer("--baseline", baseline, "--write-baseline")
        self.assertEqual(written.returncode, 0, written.stderr)
        checked = self.run_analyzer("--baseline", baseline, "--check")
        self.assertEqual(checked.returncode, 0, checked.stderr)

        self.fixture.write("src/Train/NewTarget.h", "")
        self.fixture.write(
            "src/ANT/Consumer.cpp", '#include "Target.h"\n#include "NewTarget.h"\n'
        )
        first = self.run_analyzer("--baseline", baseline, "--check")
        second = self.run_analyzer("--baseline", baseline, "--check")
        self.assertEqual(first.returncode, 1)
        self.assertEqual(first.stderr, second.stderr)
        self.assertIn("current source-module dependency inventory", first.stderr)
        self.assertIn('"target": "src/Train/NewTarget.h"', first.stderr)

        self.fixture.write("src/ANT/Consumer.cpp", "")
        stale = self.run_analyzer("--baseline", baseline, "--check")
        self.assertEqual(stale.returncode, 1)
        self.assertIn('-      "target": "src/Core/Target.h"', stale.stderr)

    def test_print_candidate_is_canonical_json(self) -> None:
        result = self.run_analyzer("--print-candidate")
        self.assertEqual(result.returncode, 0, result.stderr)
        parsed = json.loads(result.stdout)
        self.assertEqual(parsed["modules"], list(MODULES))
        self.assertEqual(result.stdout, ANALYZER_MODULE._canonical_json(parsed))

    def test_live_repository_matches_checked_in_baseline(self) -> None:
        result = subprocess.run(
            [sys.executable, str(ANALYZER), "--repository", str(REPOSITORY), "--check"],
            check=False,
            encoding="utf-8",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
