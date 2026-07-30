QT += core testlib

TEMPLATE = app
TARGET = tst_rideFileCrc

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug
DEFINES += GC_RIDE_FILE_CRC_TEST_HOOKS

SOURCES = testRideFileCrc.cpp \
          ../../../src/FileIO/RideFileCRC.cpp

HEADERS = ../../../src/FileIO/RideFileCRC.h

DISTFILES = testWindowsHeaderOrder.sh \
            windowsHeaderOrder/QtGlobal

INCLUDEPATH += ../../../src

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
