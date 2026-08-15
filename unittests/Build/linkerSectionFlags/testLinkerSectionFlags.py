#!/usr/bin/env python3

from pathlib import Path
import shutil
import subprocess
import tempfile


TEST_DIRECTORY = Path(__file__).resolve().parent
UNITTESTS = TEST_DIRECTORY.parents[1]
REPOSITORY = UNITTESTS.parent
SECTION_FLAGS = UNITTESTS / "section-gc.prf"
ZLIB_LINK = UNITTESTS / "zlib-link.prf"
QWT_MSVC_LINK = UNITTESTS / "qwt-msvc-link.prf"
RIDE_FILE_CACHE_REFRESH_STUBS = (
    UNITTESTS
    / "FileIO/rideFileCacheRefresh/RideFileCacheRefreshTestStubs.cpp"
)
RIDE_METADATA_ATOMIC_PROJECT = (
    UNITTESTS / "Metrics/rideMetadataAtomicSave/rideMetadataAtomicSave.pro"
)
RIDE_METRIC_DEPENDENCY_STUBS = (
    UNITTESTS
    / "Metrics/rideMetricDependencyGraph/RideMetricDependencyGraphTestStubs.cpp"
)
USER_METRIC_REGISTRY_STUBS = (
    UNITTESTS
    / "Metrics/userMetricRegistrySafety/UserMetricRegistrySafetyTestStubs.cpp"
)
PYTHON_CHART_PROJECT = (
    UNITTESTS / "Python/pythonChartLifecycle/pythonChartLifecycle.pro"
)
PYTHON_CHART_STUBS = (
    UNITTESTS
    / "Python/pythonChartLifecycle/PythonChartLifecycleTestStubs.cpp"
)
PYTHON_DATA_SERIES_PROJECT = (
    UNITTESTS
    / "Python/pythonDataSeriesOwnership/testPythonDataSeriesOwnership.pro"
)
PYTHON_DATA_SERIES_SOURCE = REPOSITORY / "src/Python/SIP/PythonDataSeries.cpp"
PYTHON_BINDINGS_SOURCE = REPOSITORY / "src/Python/SIP/Bindings.cpp"
APPLICATION_PROJECT = REPOSITORY / "src/src.pro"
ANT_LIFECYCLE_STUBS = (
    UNITTESTS / "Train/antLifecycle/AntLifecycleTestStubs.cpp"
)
STRAVA_ROUTES_PROJECT = (
    UNITTESTS
    / "Train/stravaRoutesDownloadPipeline/stravaRoutesDownloadPipeline.pro"
)
STRAVA_ROUTES_STUBS = (
    UNITTESTS
    / "Train/stravaRoutesDownloadPipeline/ErgFileGpxCompositionTestStubs.cpp"
)
RIDE_CACHE_REMOVAL_PROJECT = (
    UNITTESTS / "Core/rideCacheRemoval/rideCacheRemoval.pro"
)
ATOMIC_ACTIVITY_SAVE_PROJECT = (
    UNITTESTS / "FileIO/atomicActivitySave/atomicActivitySave.pro"
)
ATOMIC_ACTIVITY_SAVE_STUBS = (
    UNITTESTS / "FileIO/atomicActivitySave/RideFileTestStubs.cpp"
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
}
SHARED_RIDE_FILE_STUB_PROJECTS = {
    "FileIO/fitReaderIntegrity/fitReaderIntegrity.pro",
    "FileIO/jsonImportIntegrity/jsonImportIntegrity.pro",
    "FileIO/rideFileOwnership/rideFileOwnership.pro",
    "FileIO/tcxPointBudget/tcxPointBudget.pro",
    "FileIO/xmlImportIntegrity/xmlImportIntegrity.pro",
    "Gui/mergeActivityRidePreparation/mergeActivityRidePreparation.pro",
    "Gui/splitRideData/splitRideData.pro",
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

    ride_cache_removal = RIDE_CACHE_REMOVAL_PROJECT.read_text(
        encoding="utf-8"
    )
    for expected in (
        "include(../../unittests.pri)",
        "$${GSL_INCLUDES}",
        "$${GSL_LIBS}",
    ):
        if expected not in ride_cache_removal:
            raise AssertionError(
                "ride cache removal test is missing GSL build configuration: "
                + expected
            )

    atomic_activity_save = ATOMIC_ACTIVITY_SAVE_PROJECT.read_text(
        encoding="utf-8"
    )
    for expected in (
        "RideFileTestStubs.cpp",
        "../../../src/Core/RideCacheActivitySave.cpp",
        "win32 {",
        "LIBS += -L$$QWT_LIB_DIR -lqwt",
    ):
        if expected not in atomic_activity_save:
            raise AssertionError(
                "atomic activity save test is missing its isolated link "
                "dependency: " + expected
            )
    if "../../../src/Core/RideCache.cpp" in atomic_activity_save:
        raise AssertionError(
            "atomic activity save test compiles the complete ride cache"
        )
    atomic_activity_stubs = ATOMIC_ACTIVITY_SAVE_STUBS.read_text(
        encoding="utf-8"
    )
    for expected in (
        "bool extractSingleFile(",
        "GlobalContext *GlobalContext::context()",
        "WPrime::WPrime()",
    ):
        if expected not in atomic_activity_stubs:
            raise AssertionError(
                "atomic activity save test is missing a RideFile dependency "
                "stub: " + expected
            )

    for project in UNITTESTS.rglob("*.pro"):
        contents = project.read_text(encoding="utf-8")
        relative = project.relative_to(UNITTESTS).as_posix()
        if "$${LIBZ_LIBS}" in contents:
            raise AssertionError(
                f"{project.relative_to(UNITTESTS)} ignores the configured "
                "Windows zlib library"
            )
        if "src/FileIO/RideFile.cpp" in contents:
            if "qwt-msvc-link.prf" not in contents and "-lqwt" not in contents:
                raise AssertionError(
                    f"{relative} compiles RideFile without its MSVC Qwt "
                    "dependency"
                )
            if (relative in SHARED_RIDE_FILE_STUB_PROJECTS
                    and "RideFileTestStubs.cpp" not in contents):
                raise AssertionError(
                    f"{relative} is missing the shared RideFile link stubs"
                )
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

    qwt_msvc_link = QWT_MSVC_LINK.read_text(encoding="utf-8")
    for expected in ("win32 {", "LIBS += -L$$QWT_LIB_DIR -lqwt"):
        if expected not in qwt_msvc_link:
            raise AssertionError(
                "shared MSVC Qwt link configuration is missing " + expected
            )

    cache_stubs = RIDE_FILE_CACHE_REFRESH_STUBS.read_text(encoding="utf-8")
    for expected in (
        "AthleteSession::persistenceService() const",
        "Context::athleteSession()",
        "Specification::pass(RideItem *) const",
        "RideMetricFactory::RideMetricFactory()",
        "RideMetricFactory::instance()",
        "RideMetricFactory::rideMetric(QString) const",
        "RideItem::getWeight(int)",
    ):
        if expected not in cache_stubs:
            raise AssertionError(
                "RideFileCache MSVC link stubs are missing " + expected
            )

    ride_metadata_project = RIDE_METADATA_ATOMIC_PROJECT.read_text(
        encoding="utf-8"
    )
    if "GC_RIDE_METADATA_FILE_IO_ONLY" not in ride_metadata_project:
        raise AssertionError(
            "RideMetadata atomic-save tests compile unrelated UI code"
        )

    ride_metric_dependency_stubs = RIDE_METRIC_DEPENDENCY_STUBS.read_text(
        encoding="utf-8"
    )
    if (
        "createUserMetricForRegistry(UserMetricSettings)"
        not in ride_metric_dependency_stubs
    ):
        raise AssertionError(
            "RideMetric dependency graph test is missing its user metric "
            "factory stub"
        )

    user_metric_registry_stubs = USER_METRIC_REGISTRY_STUBS.read_text(
        encoding="utf-8"
    )
    if "DataFilter::fingerprint(QString &query)" not in user_metric_registry_stubs:
        raise AssertionError(
            "user metric registry test is missing its fingerprint stub"
        )

    python_chart_project = PYTHON_CHART_PROJECT.read_text(encoding="utf-8")
    for forbidden in (
        "../../../src/Core/Specification.cpp",
        "../../../src/Core/TimeUtils.cpp",
    ):
        if forbidden in python_chart_project:
            raise AssertionError(
                "Python chart lifecycle test compiles unrelated production "
                "code: " + forbidden
            )
    if "PythonChartLifecycleTestStubs.cpp" not in python_chart_project:
        raise AssertionError(
            "Python chart lifecycle test is missing its isolated stubs"
        )
    python_chart_stubs = PYTHON_CHART_STUBS.read_text(encoding="utf-8")
    for expected in (
        "DateRange::DateRange(QDate",
        "PlanFilter::PlanFilter(PlanFilterType type)",
        "Specification::Specification()",
    ):
        if expected not in python_chart_stubs:
            raise AssertionError(
                "Python chart lifecycle test is missing a construction stub: "
                + expected
            )

    python_data_series_project = PYTHON_DATA_SERIES_PROJECT.read_text(
        encoding="utf-8"
    )
    if "../../../src/Python/SIP/PythonDataSeries.cpp" not in python_data_series_project:
        raise AssertionError(
            "Python data-series ownership test is missing its isolated "
            "production implementation"
        )
    if "../../../src/Python/SIP/Bindings.cpp" in python_data_series_project:
        raise AssertionError(
            "Python data-series ownership test compiles unrelated bindings"
        )
    if "win32:DEFINES += Py_NO_ENABLE_SHARED" not in python_data_series_project:
        raise AssertionError(
            "Python data-series ownership test permits Windows Python autolinking"
        )
    python_data_series_source = PYTHON_DATA_SERIES_SOURCE.read_text(
        encoding="utf-8"
    )
    if "PythonDataSeries::PythonDataSeries" not in python_data_series_source:
        raise AssertionError(
            "isolated Python data-series implementation is missing constructors"
        )
    if "PythonDataSeries::PythonDataSeries" in PYTHON_BINDINGS_SOURCE.read_text(
        encoding="utf-8"
    ):
        raise AssertionError(
            "Python data-series implementation leaked back into bindings"
        )
    if "Python/SIP/PythonDataSeries.cpp" not in APPLICATION_PROJECT.read_text(
        encoding="utf-8"
    ):
        raise AssertionError(
            "production Python build omits the data-series implementation"
        )

    ant_lifecycle_stubs = ANT_LIFECYCLE_STUBS.read_text(encoding="utf-8")
    libusb_suspend = ant_lifecycle_stubs.find("#undef GC_HAVE_LIBUSB")
    sidebar_include = ant_lifecycle_stubs.find('#include "TrainSidebar.h"')
    remote_include = ant_lifecycle_stubs.find('#include "RemoteControl.h"')
    if libusb_suspend < 0 or sidebar_include < libusb_suspend:
        raise AssertionError(
            "ANT lifecycle stubs expose TrainSidebar to the production USB stack"
        )
    if 0 <= remote_include < libusb_suspend:
        raise AssertionError(
            "ANT lifecycle stubs load RemoteControl before USB isolation"
        )


if __name__ == "__main__":
    main()
