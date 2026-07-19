QT += core testlib
QT -= gui

TEMPLATE = app
TARGET = tst_dataFilterResources

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug app_bundle

SOURCES = testDataFilterResources.cpp

HEADERS = ../../../src/Core/DataFilterResources.h

INCLUDEPATH += ../../../src/Core \
               $${GSL_INCLUDES}

LIBS += $${GSL_LIBS}

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
