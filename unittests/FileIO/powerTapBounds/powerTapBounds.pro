QT += core gui widgets testlib
greaterThan(QT_MAJOR_VERSION, 5): QT += core5compat

TEMPLATE = app
TARGET = tst_powerTapBounds

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug
DEFINES += GC_POWERTAP_BOUNDS_TEST

SOURCES = testPowerTapBounds.cpp \
          ../../../src/FileIO/PowerTapDevice.cpp \
          PowerTapUtilStubs.cpp \
          ../../../src/FileIO/Device.cpp

HEADERS = ../../../src/FileIO/Device.h

INCLUDEPATH += ../../../src \
               ../../../src/Charts \
               ../../../src/Core \
               ../../../src/Gui \
               ../../../src/FileIO \
               ../../../src/Metrics \
               ../../../src/Planning \
               ../../../src/Train \
               ../../../src/Cloud \
               ../../../src/ANT

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
