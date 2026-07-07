QT += core gui widgets testlib bluetooth network xml sql svg concurrent serialport \
      multimedia multimediawidgets webenginecore webenginewidgets webchannel \
      positioning webenginequick charts openglwidgets core5compat

DEFINES += GC_HAVE_LIBUSB

TEMPLATE = app
TARGET = antLifecycle

SOURCES = testAntLifecycle.cpp \
          FakeLibUsb.cpp \
          AntLifecycleTestStubs.cpp \
          ../../../src/ANT/ANT.cpp \
          ../../../src/ANT/ANTChannel.cpp \
          ../../../src/ANT/ANTMessage.cpp \
          ../../../src/ANT/ANTlocalController.cpp \
          ../../../src/ANT/ANTLogger.cpp

HEADERS = LibUsb.h

INCLUDEPATH += $$PWD \
               $$PWD/../../../src/ANT \
               $$PWD/../../../src/Train \
               $$PWD/../../../src/FileIO \
               $$PWD/../../../src/Cloud \
               $$PWD/../../../src/Charts \
               $$PWD/../../../src/Metrics \
               $$PWD/../../../src/Gui \
               $$PWD/../../../src/Core \
               $$PWD/../../../src/Planning \
               $$PWD/../../../qwt/src

GC_OBJS = RealtimeData \
          BlinnSolver \
          CalibrationData \
          PolynomialRegression \
          RealtimeController \
          moc_ANT \
          moc_ANTChannel \
          moc_ANTlocalController \
          moc_ANTLogger \
          moc_RealtimeController

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
