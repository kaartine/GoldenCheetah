QT += testlib

TEMPLATE = app
TARGET = tst_fixGpsSmoothingSafety

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug app_bundle

SOURCES = testFixGpsSmoothingSafety.cpp

INCLUDEPATH += ../../../src

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
