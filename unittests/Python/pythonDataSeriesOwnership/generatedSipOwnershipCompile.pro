QT += core gui widgets webenginewidgets charts network sql concurrent xml core5compat

TEMPLATE = lib
TARGET = generatedSipOwnershipCompile
CONFIG += staticlib c++17

include(../../unittests.pri)
include(pythonDataSeriesOwnership.qmake)

# unittests.pri enables testcase globally. This target only verifies that the
# generated SIP translation units compile, so make check must never execute it.
CONFIG -= testcase

!equals(PYTHON_DATA_SERIES_OWNERSHIP_HAS_HEADERS, true) {
    error("Python development headers not found. Set PYTHONINCLUDES to a directory containing Python.h; this is normally required explicitly on Windows.")
}

# Archiving avoids SIP runtime linkage while compiling the generated ownership bridges.
SOURCES = ../../../src/Python/SIP/sipgoldencheetahBindings.cpp \
          ../../../src/Python/SIP/sipgoldencheetahPythonDataSeries.cpp

INCLUDEPATH += $$PYTHON_DATA_SERIES_OWNERSHIP_INCLUDEPATH
