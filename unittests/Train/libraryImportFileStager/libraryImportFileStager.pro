QT += core testlib

TEMPLATE = app
TARGET = tst_libraryImportFileStager

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testLibraryImportFileStager.cpp \
          ../../../src/Train/LibraryImportFileStager.cpp

HEADERS = ../../../src/Train/LibraryImportFileStager.h

INCLUDEPATH += ../../../src \
               ../../../src/Train

QMAKE_CXXFLAGS += -ffunction-sections -fdata-sections
QMAKE_LFLAGS += -Wl,--gc-sections

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
