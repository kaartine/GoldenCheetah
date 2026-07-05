QT += core gui testlib core5compat

TEMPLATE = app
TARGET = tst_wkoBounds
CONFIG += console testcase c++17

SOURCES = testWkoBounds.cpp \
          WkoRideFileUnderTest.cpp

INCLUDEPATH += ../../../src \
               ../../../src/Charts \
               ../../../src/Core \
               ../../../src/FileIO

include(../../unittests.pri)

sanitize {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
