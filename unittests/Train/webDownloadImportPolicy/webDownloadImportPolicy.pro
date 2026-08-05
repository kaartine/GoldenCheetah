QT += core testlib

TEMPLATE = app
TARGET = tst_webDownloadImportPolicy

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testWebDownloadImportPolicy.cpp \
          ../../../src/Train/WebDownloadImportPolicy.cpp

HEADERS = ../../../src/Train/WebDownloadImportPolicy.h

INCLUDEPATH += ../../../src \
               ../../../src/Train

include(../../section-gc.prf)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
