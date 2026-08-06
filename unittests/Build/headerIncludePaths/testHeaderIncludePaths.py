#!/usr/bin/env python3

from pathlib import Path
import re


TEST_DIRECTORY = Path(__file__).resolve().parent
REPOSITORY = TEST_DIRECTORY.parents[2]
SOURCE_ROOT = REPOSITORY / "src"
SOURCE_ROOT_DEPENDENCIES = {
    "Charts/PythonChart.h": {
        "Python/PythonChartOwner.h",
        "Python/PythonEmbed.h",
    },
}
PORTABLE_QT_SHIM = REPOSITORY / "unittests/Train/usbXpressSafety/QtPlatformShim.h"
PORTABLE_QT_PROJECT = REPOSITORY / "unittests/Train/usbXpressSafety/usbXpressSafety.pro"
INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)


def main() -> None:
    for relative_header, expected_dependencies in SOURCE_ROOT_DEPENDENCIES.items():
        header = SOURCE_ROOT / relative_header
        includes = set(INCLUDE_PATTERN.findall(header.read_text(encoding="utf-8")))
        missing = expected_dependencies - includes
        if missing:
            raise AssertionError(
                f"{relative_header} must use source-root-qualified includes: "
                + ", ".join(sorted(missing))
            )

        missing_files = {
            dependency
            for dependency in expected_dependencies
            if not (SOURCE_ROOT / dependency).is_file()
        }
        if missing_files:
            raise AssertionError(
                f"{relative_header} refers to missing source headers: "
                + ", ".join(sorted(missing_files))
            )

    shim_includes = set(
        INCLUDE_PATTERN.findall(PORTABLE_QT_SHIM.read_text(encoding="utf-8"))
    )
    if "qglobal.h" not in shim_includes or "QtCore/qglobal.h" in shim_includes:
        raise AssertionError(
            "QtPlatformShim.h must include qglobal.h through qmake's direct "
            "QtCore include path so both framework and directory Qt layouts work"
        )

    project = PORTABLE_QT_PROJECT.read_text(encoding="utf-8")
    if "QT_INSTALL_HEADERS" in project:
        raise AssertionError(
            "usbXpressSafety must use qmake's Qt module include paths instead of "
            "assuming a directory-style QT_INSTALL_HEADERS layout"
        )


if __name__ == "__main__":
    main()
