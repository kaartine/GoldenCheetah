#!/usr/bin/env python3
"""Regression coverage for qmake and modern Bison header naming."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


TEST_DIR = Path(__file__).resolve().parent
REPOSITORY_ROOT = TEST_DIR.parents[2]
PROJECT_FILE = REPOSITORY_ROOT / "src/src.pro"


def yacc_sources():
    project = PROJECT_FILE.read_text(encoding="utf-8")
    match = re.search(
        r"^YACCSOURCES\s*\+=\s*(.*?)(?:\n\s*\n)",
        project,
        re.MULTILINE | re.DOTALL,
    )
    if match is None:
        raise AssertionError("src.pro does not define YACCSOURCES")
    return [
        REPOSITORY_ROOT / "src" / token
        for token in match.group(1).replace("\\\n", " ").split()
    ]


class YaccHeaderCompatibilityTests(unittest.TestCase):
    def test_qmake_renamed_headers_resolve_from_generated_parsers(self):
        yacc = shutil.which("yacc")
        self.assertIsNotNone(yacc, "the release build requires yacc")

        sources = yacc_sources()
        self.assertTrue(sources, "YACCSOURCES must not be empty")

        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            for source in sources:
                with self.subTest(parser=source.name):
                    self.assertTrue(source.is_file(), f"missing parser source: {source}")
                    stem = source.stem
                    result = subprocess.run(
                        [yacc, "-d", "-p", stem, "-b", stem, str(source)],
                        cwd=output,
                        text=True,
                        capture_output=True,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)

                    generated_header = output / f"{stem}.tab.h"
                    generated_source = output / f"{stem}.tab.c"
                    renamed_header = output / f"{stem}_yacc.h"
                    renamed_source = output / f"{stem}_yacc.cpp"
                    generated_header.rename(renamed_header)
                    generated_source.rename(renamed_source)

                    compatibility_header = source.parent / f"{stem}.tab.h"
                    self.assertTrue(
                        compatibility_header.is_file(),
                        f"missing compatibility header: {compatibility_header}",
                    )
                    self.assertIn(
                        f'#include "{renamed_header.name}"',
                        compatibility_header.read_text(encoding="utf-8"),
                    )

                    includes = re.findall(
                        r'^\s*#\s*include\s*"([^"]+)"',
                        renamed_source.read_text(encoding="utf-8"),
                        re.MULTILINE,
                    )
                    for include in includes:
                        if not include.endswith(".tab.h"):
                            continue
                        candidates = (output / include, source.parent / include)
                        self.assertTrue(
                            any(candidate.is_file() for candidate in candidates),
                            f'{renamed_source.name} cannot resolve "{include}" '
                            "after qmake renames the generated header",
                        )


if __name__ == "__main__":
    unittest.main()
