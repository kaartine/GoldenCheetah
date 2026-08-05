QT += core testlib

TEMPLATE = app
TARGET = tst_durableFilesystem

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug
DEFINES += GC_DURABLE_FILESYSTEM_TEST_HOOKS

SOURCES = testDurableFilesystem.cpp

HEADERS = ../../../src/FileIO/AtomicFileWriter.h

INCLUDEPATH += ../../../src/FileIO

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
