QT += core testlib
QT -= gui

TEMPLATE = app
TARGET = tst_dataFilterSafety

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug app_bundle

SOURCES = testDataFilterSafety.cpp \
          ../../../src/Core/DataFilterSafety.cpp

HEADERS = ../../../src/Core/DataFilterSafety.h

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
