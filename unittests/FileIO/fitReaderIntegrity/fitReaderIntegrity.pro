QT += core gui widgets testlib xml sql network svg concurrent serialport \
      multimedia multimediawidgets webenginecore webenginewidgets webchannel \
      positioning webenginequick charts openglwidgets core5compat

TEMPLATE = app
TARGET = tst_fitReaderIntegrity

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug
DEFINES += GC_FIT_READER_ONLY

SOURCES = testFitReaderIntegrity.cpp \
          FitReaderIntegrityTestStubs.cpp \
          $$PWD/../../../src/FileIO/RideFile.cpp \
          $$PWD/../../../src/FileIO/RideFileCommand.cpp \
          $$PWD/../../../src/FileIO/FitFileIntegrity.cpp \
          $$PWD/../../../src/FileIO/FitRideFile.cpp

HEADERS = $$PWD/../../../src/FileIO/RideFile.h \
          $$PWD/../../../src/FileIO/RideFileCommand.h \
          $$PWD/../../../src/FileIO/FitFileIntegrity.h \
          $$PWD/../../../src/FileIO/FitRideFile.h

INCLUDEPATH += $$PWD/../../../src \
               $$PWD/../../../src/ANT \
               $$PWD/../../../src/Charts \
               $$PWD/../../../src/Cloud \
               $$PWD/../../../src/Core \
               $$PWD/../../../src/FileIO \
               $$PWD/../../../src/Gui \
               $$PWD/../../../src/Metrics \
               $$PWD/../../../src/Planning \
               $$PWD/../../../src/Train \
               $$PWD/../../../qwt/src \
               $$PWD/../../../contrib/qzip

QMAKE_CXXFLAGS += -ffunction-sections -fdata-sections
QMAKE_LFLAGS += -Wl,--gc-sections
LIBS += -lz

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
