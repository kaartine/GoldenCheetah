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

include(../../section-gc.prf)

sanitize:!tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}

tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=thread \
                      -fno-omit-frame-pointer \
                      -O1 \
                      -g
    QMAKE_LFLAGS += -fsanitize=thread
    linux {
        QMAKE_CXXFLAGS += -fno-pie
        QMAKE_LFLAGS += -no-pie
    }
}
