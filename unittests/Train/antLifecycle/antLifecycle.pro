QT += core gui widgets testlib bluetooth network xml sql svg concurrent serialport \
      multimedia multimediawidgets webenginecore webenginewidgets webchannel \
      positioning webenginequick charts openglwidgets core5compat

DEFINES += GC_HAVE_LIBUSB \
           GC_ANT_LIBUSB_HEADER=\\\"AntLifecycleLibUsb.h\\\"

TEMPLATE = app
TARGET = antLifecycle

SOURCES = testAntLifecycle.cpp \
          FakeLibUsb.cpp \
          AntLifecycleTestStubs.cpp \
          ../../../src/ANT/ANT.cpp \
          ../../../src/ANT/ANTChannel.cpp \
          ../../../src/ANT/ANTMessage.cpp \
          ../../../src/ANT/ANTlocalController.cpp \
          ../../../src/ANT/ANTLogger.cpp \
          ../../../src/Train/RealtimeData.cpp \
          ../../../src/Metrics/BlinnSolver.cpp \
          ../../../src/Train/CalibrationData.cpp \
          ../../../src/Train/PolynomialRegression.cpp \
          ../../../src/Train/RealtimeController.cpp

HEADERS = AntLifecycleLibUsb.h \
          ../../../src/ANT/ANT.h \
          ../../../src/ANT/ANTChannel.h \
          ../../../src/ANT/ANTlocalController.h \
          ../../../src/ANT/ANTLogger.h \
          ../../../src/Train/RealtimeController.h

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
