QT += core testlib

TEMPLATE = app
TARGET = tst_rideCacheAtomicSave

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testRideCacheAtomicSave.cpp \
          ../../../src/Core/RideCachePersistence.cpp

HEADERS = ../../../src/Core/RideCachePersistence.h \
          ../../../src/FileIO/AtomicFileWriter.h

INCLUDEPATH += ../../../src \
               ../../../src/Core \
               ../../../src/FileIO

QMAKE_CXXFLAGS += -ffunction-sections -fdata-sections
QMAKE_LFLAGS += -Wl,--gc-sections

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
