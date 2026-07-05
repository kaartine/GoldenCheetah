TEMPLATE = subdirs
CONFIG += ordered

include(../../unittests.pri)
include(pythonDataSeriesOwnership.qmake)

equals(PYTHON_DATA_SERIES_OWNERSHIP_HAS_HEADERS, true) {
    # Both projects share a directory, so keep their generated makefiles distinct.
    pythonDataSeriesOwnershipTest.file = $$PWD/testPythonDataSeriesOwnership.pro
    pythonDataSeriesOwnershipTest.makefile = Makefile.testPythonDataSeriesOwnership

    generatedSipOwnershipCompile.file = $$PWD/generatedSipOwnershipCompile.pro
    generatedSipOwnershipCompile.makefile = Makefile.generatedSipOwnershipCompile
    generatedSipOwnershipCompile.depends = pythonDataSeriesOwnershipTest

    SUBDIRS += pythonDataSeriesOwnershipTest \
               generatedSipOwnershipCompile
} else {
    equals(PYTHON_DATA_SERIES_OWNERSHIP_ALLOW_MISSING_HEADERS, true) {
        message("Skipping PythonDataSeries ownership tests by explicit request: Python development headers were not found.")
    } else {
        error("PythonDataSeries ownership tests require Python development headers. Set PYTHONINCLUDES to a directory containing Python.h, or set PYTHON_DATA_SERIES_OWNERSHIP_ALLOW_MISSING_HEADERS=true to intentionally skip this suite.")
    }
}
