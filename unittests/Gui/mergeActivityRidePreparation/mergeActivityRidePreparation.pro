QT += core gui widgets testlib xml sql network svg concurrent serialport \
      multimedia multimediawidgets webenginecore webenginewidgets webchannel \
      positioning webenginequick charts openglwidgets core5compat

TEMPLATE = app
TARGET = tst_mergeActivityRidePreparation

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testMergeActivityRidePreparation.cpp \
          MergeActivityRidePreparationTestStubs.cpp \
          ../../../src/Gui/MergeActivityRidePreparation.cpp \
          ../../../src/FileIO/RideFile.cpp \
          ../../../src/FileIO/RideFileCommand.cpp \
          ../../../src/Core/SplineLookup.cpp

HEADERS = ../../../src/Gui/MergeActivityRidePreparation.h \
          ../../../src/FileIO/RideFile.h \
          ../../../src/FileIO/RideFileCommand.h \
          ../../../src/Core/SplineLookup.h

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

QWT_LIB_DIR = $$OUT_PWD/../../../qwt/lib
LIBS += -L$$QWT_LIB_DIR -lqwt -lz
QMAKE_RPATHDIR += $$QWT_LIB_DIR

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}

leakcheck:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=leak -fno-omit-frame-pointer
    QMAKE_LFLAGS += -fsanitize=leak
}
