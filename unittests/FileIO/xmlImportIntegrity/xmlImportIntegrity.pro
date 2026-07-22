QT += core gui widgets testlib xml sql network svg concurrent serialport \
      multimedia multimediawidgets webenginecore webenginewidgets webchannel \
      positioning webenginequick charts openglwidgets core5compat

TEMPLATE = app
TARGET = tst_xmlImportIntegrity

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug
DEFINES += GC_TCX_READER_ONLY GC_FITLOG_READER_ONLY

SOURCES = testXmlImportIntegrity.cpp \
          XmlImportIntegrityTestStubs.cpp \
          ../../../src/FileIO/RideFile.cpp \
          ../../../src/FileIO/RideFileCommand.cpp \
          ../../../src/FileIO/TcxParser.cpp \
          ../../../src/FileIO/TcxRideFile.cpp \
          ../../../src/FileIO/GpxParser.cpp \
          ../../../src/FileIO/GpxRideFile.cpp \
          ../../../src/FileIO/FitlogParser.cpp \
          ../../../src/FileIO/FitlogRideFile.cpp

HEADERS = ../../../src/FileIO/RideFile.h \
          ../../../src/FileIO/RideFileCommand.h \
          ../../../src/FileIO/TcxParser.h \
          ../../../src/FileIO/TcxRideFile.h \
          ../../../src/FileIO/GpxParser.h \
          ../../../src/FileIO/GpxRideFile.h \
          ../../../src/FileIO/FitlogParser.h \
          ../../../src/FileIO/FitlogRideFile.h

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
