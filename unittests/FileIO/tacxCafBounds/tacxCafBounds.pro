QT += core testlib

TEMPLATE = app
TARGET = tst_tacxCafBounds
CONFIG += console testcase c++17

SOURCES = testTacxCafBounds.cpp \
          TacxCafRideFileUnderTest.cpp

INCLUDEPATH += ../../../src \
               ../../../src/FileIO

include(../../unittests.pri)

CONFIG -= debug debug_and_release
CONFIG += release
DEFINES += QT_NO_DEBUG

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
