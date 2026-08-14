QT += core gui widgets testlib xml sql network svg concurrent serialport \
      multimedia multimediawidgets webenginecore webenginewidgets webchannel \
      positioning webenginequick charts openglwidgets core5compat

TEMPLATE = app
TARGET = tst_splitRideData

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testSplitRideData.cpp \
          SplitRideDataTestStubs.cpp \
          ../../FileIO/atomicActivitySave/RideFileTestStubs.cpp \
          ../../FileIO/RideFileSettingsTestStubs.cpp \
          ../../../src/Gui/SplitRideData.cpp \
          ../../../src/FileIO/RideFile.cpp \
          ../../../src/FileIO/RideFileCRC.cpp \
          ../../../src/FileIO/RideFileCommand.cpp

HEADERS = ../../../src/Gui/SplitActivityWorkflow.h \
          ../../../src/Gui/SplitRideData.h \
          ../../../src/FileIO/RideFile.h \
          ../../../src/FileIO/RideFileCommand.h

INCLUDEPATH += ../../../src \
               ../../../src/ANT \
               ../../../src/Charts \
               ../../../src/Cloud \
               ../../../src/Core \
               ../../../src/FileIO \
               ../../../src/Gui \
               ../../../src/Metrics \
               ../../../src/Planning \
               ../../../src/Train \
               ../../../qwt/src \
               ../../../contrib/qzip

include(../../section-gc.prf)
include(../../zlib-link.prf)
include(../../qwt-msvc-link.prf)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
