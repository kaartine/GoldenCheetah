#!/usr/bin/env python3

from pathlib import Path
import shutil
import subprocess
import tempfile


TEST_DIRECTORY = Path(__file__).resolve().parent
UNITTESTS = TEST_DIRECTORY.parents[1]
SECTION_FLAGS = UNITTESTS / "section-gc.prf"
PORTABLE_SECTION_PROJECTS = (
    "Charts/mapPageSecurity/mapPageSecurity.pro",
    "Charts/mapRoutePointIndex/mapRoutePointIndex.pro",
    "Core/athleteMigrationSafety/athleteMigrationSafety.pro",
    "Core/credentialSettings/credentialSettings.pro",
    "Core/measuresAtomicSave/measuresAtomicSave.pro",
    "Core/rideCacheAtomicSave/rideCacheAtomicSave.pro",
    "FileIO/fitReaderIntegrity/fitReaderIntegrity.pro",
    "FileIO/jsonImportIntegrity/jsonImportIntegrity.pro",
    "FileIO/rideFileCacheRefresh/rideFileCacheRefresh.pro",
    "FileIO/rideFileOwnership/rideFileOwnership.pro",
    "FileIO/tcxPointBudget/tcxPointBudget.pro",
    "FileIO/xmlImportIntegrity/xmlImportIntegrity.pro",
    "Gui/mergeActivityRidePreparation/mergeActivityRidePreparation.pro",
    "Gui/splitRideData/splitRideData.pro",
    "Metrics/rideMetadataAtomicSave/rideMetadataAtomicSave.pro",
    "Python/pythonChartLifecycle/pythonChartLifecycle.pro",
    "Train/libraryImportFileStager/libraryImportFileStager.pro",
    "Train/webDownloadImportPolicy/webDownloadImportPolicy.pro",
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

    apple = generated_flags("apple")
    require_flag(apple, "-Wl,-dead_strip", "--gc-sections", "Apple")

    gnu = generated_flags("gnu")
    require_flag(gnu, "-Wl,--gc-sections", "-dead_strip", "GNU")

    msvc = generated_flags("msvc")
    for forbidden in ("--gc-sections", "-dead_strip", "-ffunction-sections"):
        if forbidden in msvc:
            raise AssertionError(f"MSVC config contains forbidden {forbidden}")


if __name__ == "__main__":
    main()
