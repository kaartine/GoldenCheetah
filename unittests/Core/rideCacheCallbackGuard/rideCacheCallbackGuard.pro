QT += core testlib

TEMPLATE = app
TARGET = tst_rideCacheCallbackGuard

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testRideCacheCallbackGuard.cpp

HEADERS = ../../../src/Core/RideCacheCallbackGuard.h

INCLUDEPATH += ../../../src/Core

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
