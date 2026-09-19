QT += core gui widgets testlib xml sql network concurrent serialport \
      webenginecore webenginewidgets webchannel \
      positioning webenginequick charts openglwidgets core5compat

TEMPLATE = app
TARGET = tst_rideRefreshMeasures

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testRideRefreshMeasures.cpp \
          ../../../src/Core/RideRefreshMeasures.cpp \
          ../../../src/Core/RideRefreshMeasuresCapture.cpp \
          ../../../src/Core/Measures.cpp

HEADERS = ../../../src/Core/RideRefreshMeasures.h \
          ../../../src/Core/Measures.h \
          ../../../src/FileIO/AtomicFileWriter.h

INCLUDEPATH += ../../../src ../../../src/ANT ../../../src/Charts \
               ../../../src/Cloud ../../../src/Core ../../../src/FileIO \
               ../../../src/Gui ../../../src/Metrics ../../../src/Planning \
               ../../../src/Train ../../../qwt/src

include(../../section-gc.prf)
sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
