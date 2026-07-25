QT += core testlib

TEMPLATE = app
TARGET = tst_fitImportIntegrity

include(../../unittests.pri)

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
