QT += core testlib

TEMPLATE = app
TARGET = tst_rideFileCacheWriteError

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testRideFileCacheWriteError.cpp \
          ../../../src/FileIO/RideFileCacheWriteError.cpp

HEADERS = ../../../src/FileIO/RideFileCacheWriteError.h

INCLUDEPATH += ../../../src/FileIO

sanitize:!tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}

tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=thread -fno-omit-frame-pointer
    QMAKE_LFLAGS += -fsanitize=thread
}
