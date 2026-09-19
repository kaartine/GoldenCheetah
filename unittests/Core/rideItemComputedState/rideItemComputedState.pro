QT += core gui widgets testlib core5compat

TEMPLATE = app
TARGET = tst_rideItemComputedState

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug
DEFINES += GC_RIDE_ITEM_REFRESH_TEST_HOOKS

SOURCES = testRideItemComputedState.cpp \
          ../../../src/Core/RideCacheSnapshot.cpp

HEADERS = ../../../src/Core/RideCacheSnapshot.h \
          ../../../src/Core/RideCacheStartup.h \
          ../../../src/Core/RideItem.h \
          ../../../src/FileIO/RideFile.h

INCLUDEPATH += ../../../src \
               ../../../src/Charts \
               ../../../src/Core \
               ../../../src/FileIO \
               ../../../src/Gui \
               ../../../src/Metrics \
               ../../../src/Train

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
