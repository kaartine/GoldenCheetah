QT += core testlib

TEMPLATE = app
TARGET = tst_rideFileCacheIntegrity
CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testRideFileCacheIntegrity.cpp \
          ../../../src/FileIO/RideFileCRC.cpp \
          ../../../src/FileIO/RideFileCacheIntegrity.cpp

HEADERS = ../../../src/FileIO/RideFileCRC.h \
          ../../../src/FileIO/RideFileCacheIntegrity.h

INCLUDEPATH += ../../../src/FileIO

include(../../unittests.pri)

sanitize:!tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}

linux:tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=thread \
                      -fno-omit-frame-pointer \
                      -fno-pie \
                      -O1 \
                      -g
    QMAKE_LFLAGS += -fsanitize=thread \
                    -no-pie
}
