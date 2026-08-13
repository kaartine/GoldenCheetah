#!/usr/bin/env python3
"""Regression coverage for qmake shadow-build artifact paths."""

from pathlib import Path
import re
import unittest


TEST_DIR = Path(__file__).resolve().parent
REPOSITORY_ROOT = TEST_DIR.parents[2]
APPLICATION_PROJECT = REPOSITORY_ROOT / "src/src.pro"
QWT_PROJECT = REPOSITORY_ROOT / "qwt/src/src.pro"


class ShadowBuildPathTests(unittest.TestCase):
    def test_application_links_the_qwt_shadow_build_output(self):
        qwt_project = QWT_PROJECT.read_text(encoding="utf-8")
        self.assertRegex(
            qwt_project,
            r"(?m)^QWT_OUT_ROOT\s*=\s*\$\$\{OUT_PWD\}/\.\.$",
        )
        self.assertRegex(
            qwt_project,
            r"(?m)^DESTDIR\s*=\s*\$\$\{QWT_OUT_ROOT\}/lib$",
        )

        application_project = APPLICATION_PROJECT.read_text(encoding="utf-8")
        qwt_link_roots = re.findall(
            r"(?m)^\s*LIBS\s*\+=\s*-L\$\$\{(\w+)\}/\.\./qwt/lib\s+-lqwtd?\s*$",
            application_project,
        )
        self.assertEqual(
            qwt_link_roots,
            ["OUT_PWD", "OUT_PWD", "OUT_PWD"],
            "release and debug links must resolve Qwt from the shadow build root",
        )


if __name__ == "__main__":
    unittest.main()
