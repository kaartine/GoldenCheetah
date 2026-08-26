QT += core testlib

TEMPLATE = app
TARGET = tst_fitImportIntegrity

include(../../unittests.pri)

GC_TEST_SOURCE_ROOT = $$clean_path($$_PRO_FILE_PWD_/../../..)
DEFINES += GC_TEST_SOURCE_ROOT=\\\"$${GC_TEST_SOURCE_ROOT}\\\"

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testFitImportIntegrity.cpp \
          $$PWD/../../../src/FileIO/FitFileIntegrity.cpp

HEADERS = $$PWD/../../../src/FileIO/FitFileIntegrity.h

INCLUDEPATH += $$PWD/../../../src

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
