QT += core testlib

TEMPLATE = app
TARGET = tst_rideRefreshRoutes

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

include(../../section-gc.prf)

SOURCES = testRideRefreshRoutes.cpp \
          ../../../src/Core/RideRefreshRoutes.cpp \
          ../../../src/Core/RideRefreshRoutesAssembly.cpp

GC_TEST_SOURCE_ROOT = $$clean_path($$_PRO_FILE_PWD_/../../..)
DEFINES += GC_TEST_SOURCE_ROOT="$${GC_TEST_SOURCE_ROOT}"

HEADERS = ../../../src/Core/RideRefreshRoutes.h

INCLUDEPATH += ../../../src ../../../src/Core ../../../src/FileIO \
               ../../../src/Gui ../../../src/Metrics ../../../qwt/src

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
