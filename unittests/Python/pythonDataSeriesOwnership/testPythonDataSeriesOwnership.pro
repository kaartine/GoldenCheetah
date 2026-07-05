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
          ../../../src/Python/SIP/Bindings.cpp

INCLUDEPATH += $$PYTHON_DATA_SERIES_OWNERSHIP_INCLUDEPATH

# Qt's cl and clang-cl mkspecs both include msvc in QMAKE_COMPILER. Test it
# first so only native Clang and GCC can enter GNU-style option branches.
PYTHON_DATA_SERIES_OWNERSHIP_TOOLCHAIN = unsupported
PYTHON_DATA_SERIES_OWNERSHIP_GNU_FLAGS = false
contains(QMAKE_COMPILER, msvc) {
    PYTHON_DATA_SERIES_OWNERSHIP_TOOLCHAIN = msvc
} else:contains(QMAKE_COMPILER, clang) {
    PYTHON_DATA_SERIES_OWNERSHIP_TOOLCHAIN = clang
    PYTHON_DATA_SERIES_OWNERSHIP_GNU_FLAGS = true
} else:contains(QMAKE_COMPILER, gcc) {
    PYTHON_DATA_SERIES_OWNERSHIP_TOOLCHAIN = gcc
    PYTHON_DATA_SERIES_OWNERSHIP_GNU_FLAGS = true
}

equals(PYTHON_DATA_SERIES_OWNERSHIP_TOOLCHAIN, msvc) {
    QMAKE_CXXFLAGS += /Gy
    QMAKE_LFLAGS += /OPT:REF
}

equals(PYTHON_DATA_SERIES_OWNERSHIP_GNU_FLAGS, true) {
    QMAKE_CXXFLAGS += -ffunction-sections -fdata-sections
    macx {
        QMAKE_LFLAGS += -Wl,-dead_strip
    } else {
        QMAKE_LFLAGS += -Wl,--gc-sections
    }
}

sanitize {
    equals(PYTHON_DATA_SERIES_OWNERSHIP_GNU_FLAGS, true) {
        # vptr metadata retains RTTI from unrelated Bindings.cpp sections.
        QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                          -fno-omit-frame-pointer \
                          -fno-sanitize=vptr \
                          -fno-sanitize-recover=all
        QMAKE_LFLAGS += -fsanitize=address,undefined
    } else:equals(PYTHON_DATA_SERIES_OWNERSHIP_TOOLCHAIN, msvc) {
        error("CONFIG+=sanitize is unsupported for MSVC-style toolchains, including clang-cl")
    } else {
        error("CONFIG+=sanitize requires GCC or native Clang")
    }
}
