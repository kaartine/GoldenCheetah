QT += core testlib

TEMPLATE = app
TARGET = tst_rideCachePerformance

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testRideCachePerformance.cpp

HEADERS = ../../../src/Core/RideCacheAggregate.h \
          ../../../src/Core/RideCacheStartup.h

INCLUDEPATH += ../../../src \
               ../../../src/Core

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}

leakcheck:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=leak -fno-omit-frame-pointer
    QMAKE_LFLAGS += -fsanitize=leak
}
