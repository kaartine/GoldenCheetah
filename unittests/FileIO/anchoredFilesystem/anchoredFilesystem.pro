QT += core testlib

TEMPLATE = app
TARGET = tst_anchoredFilesystem

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testAnchoredFilesystem.cpp \
          ../../../src/FileIO/AnchoredFileSystem.cpp

HEADERS = ../../../src/FileIO/AnchoredFileSystem.h

INCLUDEPATH += ../../../src/FileIO

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}

thread_sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=thread \
                      -fno-omit-frame-pointer \
                      -fPIE
    QMAKE_LFLAGS += -fsanitize=thread -pie
}
