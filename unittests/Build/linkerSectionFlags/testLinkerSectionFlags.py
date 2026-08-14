#!/usr/bin/env python3

from pathlib import Path
import shutil
import subprocess
import tempfile


TEST_DIRECTORY = Path(__file__).resolve().parent
UNITTESTS = TEST_DIRECTORY.parents[1]
SECTION_FLAGS = UNITTESTS / "section-gc.prf"
ZLIB_LINK = UNITTESTS / "zlib-link.prf"
STRAVA_ROUTES_PROJECT = (
    UNITTESTS
    / "Train/stravaRoutesDownloadPipeline/stravaRoutesDownloadPipeline.pro"
)
STRAVA_ROUTES_STUBS = (
    UNITTESTS
    / "Train/stravaRoutesDownloadPipeline/ErgFileGpxCompositionTestStubs.cpp"
)
PORTABLE_SECTION_PROJECTS = (
    "Charts/mapPageSecurity/mapPageSecurity.pro",
    "Charts/mapRoutePointIndex/mapRoutePointIndex.pro",
    "Charts/powerHistSelection/powerHistSelection.pro",
    "Core/athleteMigrationSafety/athleteMigrationSafety.pro",
    "Core/credentialSettings/credentialSettings.pro",
    "Core/measuresAtomicSave/measuresAtomicSave.pro",
    "Core/plannedActivityFileStager/plannedActivityFileStager.pro",
    "Core/rideCacheAtomicSave/rideCacheAtomicSave.pro",
    "Core/rideCacheRemoval/rideCacheRemoval.pro",
    "FileIO/fitReaderIntegrity/fitReaderIntegrity.pro",
    "FileIO/jsonImportIntegrity/jsonImportIntegrity.pro",
    "FileIO/rideFileCacheRefresh/rideFileCacheRefresh.pro",
    "FileIO/rideFileOwnership/rideFileOwnership.pro",
    "FileIO/tcxPointBudget/tcxPointBudget.pro",
    "FileIO/xmlImportIntegrity/xmlImportIntegrity.pro",
    "Gui/mergeActivityRidePreparation/mergeActivityRidePreparation.pro",
    "Gui/rideNavigatorProxyMapping/rideNavigatorProxyMapping.pro",
    "Gui/splitRideData/splitRideData.pro",
    "Metrics/rideMetricDependencyGraph/rideMetricDependencyGraph.pro",
    "Metrics/rideMetadataAtomicSave/rideMetadataAtomicSave.pro",
    "Metrics/userMetricRegistrySafety/userMetricRegistrySafety.pro",
    "Python/pythonChartLifecycle/pythonChartLifecycle.pro",
    "Train/libraryImportFileStager/libraryImportFileStager.pro",
    "Train/stravaRoutesDownloadPipeline/stravaRoutesDownloadPipeline.pro",
    "Train/webDownloadImportPolicy/webDownloadImportPolicy.pro",
)
CUSTOM_SECTION_PROJECTS = {
    "Core/rideCacheSaveSnapshot/rideCacheSaveSnapshot.pro",
    "FileIO/atomicActivitySave/atomicActivitySave.pro",
    "Python/pythonDataSeriesOwnership/testPythonDataSeriesOwnership.pro",
}
SECTION_FLAG_MARKERS = (
    "-ffunction-sections",
    "-fdata-sections",
    "--gc-sections",
    "-dead_strip",
    "/Gy",
)


def qmake_executable() -> str:
    for candidate in ("qmake6", "qmake"):
        executable = shutil.which(candidate)
        if executable:
            return executable
    raise AssertionError("qmake is required to validate generated linker flags")


def generated_flags(platform: str) -> str:
    with tempfile.TemporaryDirectory(prefix=f"gc-section-flags-{platform}-") as temporary:
        root = Path(temporary)
        project = root / "flags.pro"
        project.write_text(
            "TEMPLATE = app\n"
            "TARGET = section_flags_fixture\n"
            "SOURCES = main.cpp\n"
            f"include({SECTION_FLAGS.as_posix()})\n",
            encoding="utf-8",
        )
        (root / "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
        result = subprocess.run(
            [
                qmake_executable(),
                f"GC_SECTION_LINKER_OVERRIDE={platform}",
                project.name,
            ],
            cwd=root,
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise AssertionError(f"qmake failed for {platform}: {result.stderr}")
        makefiles = sorted(root.glob("Makefile*"))
        if not makefiles:
            raise AssertionError(f"qmake generated no Makefile for {platform}")
        return "\n".join(
            makefile.read_text(encoding="utf-8", errors="replace")
            for makefile in makefiles
        )


def generated_zlib_flags() -> str:
    with tempfile.TemporaryDirectory(prefix="gc-zlib-flags-") as temporary:
        root = Path(temporary)
        project = root / "zlib.pro"
        project.write_text(
            "TEMPLATE = app\n"
            "TARGET = zlib_fixture\n"
            "SOURCES = main.cpp\n"
            f"include({ZLIB_LINK.as_posix()})\n",
            encoding="utf-8",
        )
        (root / "main.cpp").write_text(
            "int main() { return 0; }\n", encoding="utf-8"
        )
        result = subprocess.run(
            [
                qmake_executable(),
                "ZLIB_LIBS=-LC:/vcpkg/lib -lzlib",
                project.name,
            ],
            cwd=root,
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise AssertionError(
                "qmake failed for configured zlib: " + result.stderr
            )
        return (root / "Makefile").read_text(
            encoding="utf-8", errors="replace"
        )


def require_flag(flags: str, expected: str, forbidden: str, platform: str) -> None:
    if expected not in flags:
        raise AssertionError(f"{platform} config is missing {expected}")
    if forbidden in flags:
        raise AssertionError(f"{platform} config contains forbidden {forbidden}")


def main() -> None:
    if not SECTION_FLAGS.is_file():
        raise AssertionError("shared section-gc.prf configuration is missing")

    missing_includes = []
    for relative_path in PORTABLE_SECTION_PROJECTS:
        project = UNITTESTS / relative_path
        if "include(../../section-gc.prf)" not in project.read_text(encoding="utf-8"):
            missing_includes.append(relative_path)
    if missing_includes:
        raise AssertionError(
            "unit projects do not use portable section GC flags: "
            + ", ".join(missing_includes)
        )

    manual_section_projects = {
        project.relative_to(UNITTESTS).as_posix()
        for project in UNITTESTS.rglob("*.pro")
        if any(
            marker in project.read_text(encoding="utf-8")
            for marker in SECTION_FLAG_MARKERS
        )
    }
    unexpected_manual_projects = manual_section_projects - CUSTOM_SECTION_PROJECTS
    missing_custom_projects = CUSTOM_SECTION_PROJECTS - manual_section_projects
    if unexpected_manual_projects:
        raise AssertionError(
            "unit projects define section GC flags outside section-gc.prf: "
            + ", ".join(sorted(unexpected_manual_projects))
        )
    if missing_custom_projects:
        raise AssertionError(
            "stale custom section GC project allowlist: "
            + ", ".join(sorted(missing_custom_projects))
        )

    apple = generated_flags("apple")
    require_flag(apple, "-Wl,-dead_strip", "--gc-sections", "Apple")

    gnu = generated_flags("gnu")
    require_flag(gnu, "-Wl,--gc-sections", "-dead_strip", "GNU")

    msvc = generated_flags("msvc")
    for expected in ("/Gy", "/OPT:REF"):
        if expected not in msvc:
            raise AssertionError(f"MSVC config is missing {expected}")
    for forbidden in ("--gc-sections", "-dead_strip", "-ffunction-sections"):
        if forbidden in msvc:
            raise AssertionError(f"MSVC config contains forbidden {forbidden}")

    strava_project = STRAVA_ROUTES_PROJECT.read_text(encoding="utf-8")
    for expected in ("../../../qwt/lib -lqwt",):
        if expected not in strava_project:
            raise AssertionError(
                "Strava routes test is missing its MSVC link dependency: "
                + expected
            )
    if "../../../src/Core/TimeUtils.cpp" in strava_project:
        raise AssertionError(
            "Strava routes test compiles the unrelated TimeUtils widget implementation"
        )
    strava_stubs = STRAVA_ROUTES_STUBS.read_text(encoding="utf-8")
    for expected in (
        "bool extractSingleFile(",
        "convertToLocalTime(QString timestamp)",
        "NS_TTSReader::TTSReader::parseFile",
        "ZwoParser::startDocument",
    ):
        if expected not in strava_stubs:
            raise AssertionError(
                "Strava routes test is missing a production dependency stub: "
                + expected
            )

    for project in UNITTESTS.rglob("*.pro"):
        contents = project.read_text(encoding="utf-8")
        if "src/FileIO/AnchoredFileSystem.cpp" not in contents:
            continue
        if "-ladvapi32" not in contents:
            raise AssertionError(
                f"{project.relative_to(UNITTESTS)} compiles AnchoredFileSystem "
                "without linking advapi32 on Windows"
            )

        for line in contents.splitlines():
            if not line.startswith("LIBS +="):
                continue
            if "-lz" not in line.split():
                continue
            if "include(../../zlib-link.prf)" not in contents:
                raise AssertionError(
                    f"{project.relative_to(UNITTESTS)} links zlib without "
                    "honoring the configured Windows library"
                )

    zlib_link = ZLIB_LINK.read_text(encoding="utf-8")
    for expected in ("isEmpty(ZLIB_LIBS)", "LIBS += $${ZLIB_LIBS}"):
        if expected not in zlib_link:
            raise AssertionError(
                "shared zlib link configuration is missing " + expected
            )
    configured_zlib = generated_zlib_flags()
    if "-LC:/vcpkg/lib -lzlib" not in configured_zlib:
        raise AssertionError(
            "shared zlib link configuration ignores ZLIB_LIBS"
        )


if __name__ == "__main__":
    main()
