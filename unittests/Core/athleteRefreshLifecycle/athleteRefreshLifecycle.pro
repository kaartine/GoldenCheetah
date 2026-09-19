QT += core testlib

TEMPLATE = app
TARGET = tst_athleteRefreshLifecycle

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug
GC_TEST_SOURCE_ROOT = $$clean_path($$_PRO_FILE_PWD_/../../..)
DEFINES += GC_TEST_SOURCE_ROOT=\"$${GC_TEST_SOURCE_ROOT}\"

SOURCES = testAthleteRefreshLifecycle.cpp \
          ../../../src/Core/AthleteRefreshLifecycle.cpp

HEADERS = ../../../src/Core/AthleteRefreshLifecycle.h

INCLUDEPATH += ../../../src/Core

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
