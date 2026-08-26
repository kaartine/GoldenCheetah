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
TEST_SOURCE_DEPENDENCIES = {
    "../../../src/Train/RealtimeController.cpp": {
        "../../../src/Train/WorkoutRideTargetPlanner.cpp",
    },
    "../../../src/Train/BT40Device.cpp": {
        "../../../src/Train/BluetoothTrainerCapabilities.cpp",
    },
}


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
    if "QtCore/qglobal.h" not in shim_includes or "qglobal.h" in shim_includes:
        raise AssertionError(
            "QtPlatformShim.h must use the module-qualified QtCore/qglobal.h "
            "include supported by framework and directory Qt layouts"
        )

    project = PORTABLE_QT_PROJECT.read_text(encoding="utf-8")
    expected_force_include = """unix {
    macx {
        QMAKE_CXXFLAGS += -F$$[QT_INSTALL_LIBS]
    } else {
        QMAKE_CXXFLAGS += -I$$[QT_INSTALL_HEADERS]
    }

    QMAKE_CXXFLAGS += -include $$PWD/QtPlatformShim.h
}"""
    if expected_force_include not in project:
        raise AssertionError(
            "usbXpressSafety must give the compiler-forced Qt shim a direct "
            "QtCore header path for framework and directory Qt layouts"
        )

    for project_path in sorted((REPOSITORY / "unittests").rglob("*.pro")):
        project_text = project_path.read_text(encoding="utf-8")
        for linked_source, required_sources in TEST_SOURCE_DEPENDENCIES.items():
            if linked_source not in project_text:
                continue
            missing_sources = {
                source for source in required_sources if source not in project_text
            }
            if missing_sources:
                relative_project = project_path.relative_to(REPOSITORY)
                raise AssertionError(
                    f"{relative_project} is missing linked production sources: "
                    + ", ".join(sorted(missing_sources))
                )


if __name__ == "__main__":
    main()
