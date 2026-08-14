QT += core gui widgets testlib xml sql network svg concurrent serialport \
      multimedia multimediawidgets webenginecore webenginewidgets webchannel \
      positioning webenginequick charts openglwidgets core5compat

TEMPLATE = app
TARGET = tst_plannedActivityFileStager

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testPlannedActivityFileStager.cpp \
          PlannedActivityFileStagerTestStubs.cpp \
          ../../../src/Core/PlannedActivityFileStager.cpp \
          ../../../src/FileIO/RideFile.cpp \
          ../../../src/FileIO/RideFileCRC.cpp \
          ../../../src/FileIO/RideFileCommand.cpp

HEADERS = ../../../src/Core/PlannedActivityFileStager.h \
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
QWT_LIB_DIR = $$OUT_PWD/../../../qwt/lib
LIBS += -L$$QWT_LIB_DIR -lqwt
include(../../zlib-link.prf)
QMAKE_RPATHDIR += $$QWT_LIB_DIR

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
