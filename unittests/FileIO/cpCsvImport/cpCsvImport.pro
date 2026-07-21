QT += core testlib

TEMPLATE = app
TARGET = tst_cpCsvImport
CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testCpCsvImport.cpp \
          ../../../src/FileIO/CpCsvImport.cpp

HEADERS = ../../../src/FileIO/CpCsvImport.h

INCLUDEPATH += ../../../src/FileIO

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
