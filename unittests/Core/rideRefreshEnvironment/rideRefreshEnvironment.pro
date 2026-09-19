QT += core gui testlib

TEMPLATE = app
TARGET = tst_rideRefreshEnvironment

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testRideRefreshEnvironment.cpp \
          ../../../src/Core/AthleteRefreshLifecycle.cpp \
          ../../../src/Core/AthleteSession.cpp \
          ../../../src/Core/RideRefreshEnvironment.cpp \
          ../../../src/Core/RideRefreshMeasures.cpp \
          ../../../src/Core/RideRefreshRoutes.cpp \
          ../../../src/Core/RideRefreshZones.cpp \
          ../../../src/Core/Units.cpp

HEADERS = ../../../src/Core/AthleteRefreshLifecycle.h \
          ../../../src/Core/AthleteSession.h \
          ../../../src/Core/RideRefreshEnvironment.h \
          ../../../src/Core/RideRefreshMeasures.h \
          ../../../src/Core/RideRefreshRoutes.h \
          ../../../src/Core/RideRefreshZones.h \
          ../../../src/Core/SessionServices.h

INCLUDEPATH += ../../../src/Core

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
