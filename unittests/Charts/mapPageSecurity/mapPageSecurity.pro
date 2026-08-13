QT += core testlib

TEMPLATE = app
TARGET = tst_mapPageSecurity

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testMapPageSecurityPolicy.cpp \
          ../../../src/Charts/MapPageSecurityPolicy.cpp

HEADERS = ../../../src/Charts/MapPageSecurityPolicy.h

RESOURCES = ../../../src/Resources/map.qrc

INCLUDEPATH += ../../../src \
               ../../../src/Charts

include(../../section-gc.prf)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
