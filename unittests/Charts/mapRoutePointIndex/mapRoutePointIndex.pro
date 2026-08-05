QT += core testlib

TEMPLATE = app
TARGET = tst_mapRoutePointIndex

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testMapRoutePointIndex.cpp \
          ../../../src/Charts/MapRoutePointIndex.cpp

HEADERS = ../../../src/Charts/MapRoutePointIndex.h

INCLUDEPATH += ../../../src \
               ../../../src/Charts

QMAKE_CXXFLAGS += -ffunction-sections -fdata-sections
QMAKE_LFLAGS += -Wl,--gc-sections

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
