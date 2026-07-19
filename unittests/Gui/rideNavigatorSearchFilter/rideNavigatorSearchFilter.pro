QT += core testlib
QT -= gui

TEMPLATE = app
TARGET = tst_rideNavigatorSearchFilter

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug app_bundle

SOURCES = testRideNavigatorSearchFilter.cpp

HEADERS = ../../../src/Gui/RideNavigatorSearchFilter.h

INCLUDEPATH += ../../../src/Gui

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
