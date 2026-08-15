QT += core gui widgets testlib webenginewidgets charts network sql concurrent xml core5compat

TEMPLATE = app
TARGET = testPythonDataSeriesOwnership
CONFIG += console testcase c++17

include(../../unittests.pri)
include(pythonDataSeriesOwnership.qmake)

!equals(PYTHON_DATA_SERIES_OWNERSHIP_HAS_HEADERS, true) {
    error("Python development headers not found. Set PYTHONINCLUDES to a directory containing Python.h; this is normally required explicitly on Windows.")
}

SOURCES = testPythonDataSeriesOwnership.cpp \
          ../../../src/Python/SIP/PythonDataSeries.cpp

INCLUDEPATH += $$PYTHON_DATA_SERIES_OWNERSHIP_INCLUDEPATH

sanitize {
    contains(QMAKE_COMPILER, msvc) {
        error("CONFIG+=sanitize is unsupported for MSVC-style toolchains, including clang-cl")
    } else {
        QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                          -fno-omit-frame-pointer \
                          -fno-sanitize-recover=all
        QMAKE_LFLAGS += -fsanitize=address,undefined
    }
}
