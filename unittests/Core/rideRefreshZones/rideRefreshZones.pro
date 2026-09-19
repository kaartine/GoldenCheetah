QT += concurrent core gui network sql testlib widgets xml \
      positioning webenginequick charts openglwidgets
greaterThan(QT_MAJOR_VERSION, 5): QT += core5compat

TEMPLATE = app
TARGET = tst_rideRefreshZones

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug
GC_TEST_SOURCE_ROOT = $$clean_path($$_PRO_FILE_PWD_/../../..)
DEFINES += GC_TEST_SOURCE_ROOT="$${GC_TEST_SOURCE_ROOT}"
include(../../section-gc.prf)

SOURCES = testRideRefreshZones.cpp \
          ../../../src/Core/RideRefreshZones.cpp \
          ../../../src/Core/RideRefreshZonesCapture.cpp \
          ../../../src/Core/RideRefreshZonesAssembly.cpp \
          ../../../src/Metrics/Zones.cpp \
          ../../../src/Metrics/HrZones.cpp \
          ../../../src/Metrics/PaceZones.cpp \
          ../../../src/Core/Units.cpp

HEADERS = ../../../src/Core/RideRefreshZones.h \
          ../../../src/Core/Units.h \
          ../../../src/Metrics/Zones.h \
          ../../../src/Metrics/HrZones.h \
          ../../../src/Metrics/PaceZones.h

INCLUDEPATH += ../../../src/Core ../../../src/Metrics ../../../src/Gui \
               ../../../src/Charts ../../../src/FileIO ../../../src/Train \
               ../../../src/ANT ../../../src/Cloud ../../../src/Planning \
               ../../../qwt/src

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
