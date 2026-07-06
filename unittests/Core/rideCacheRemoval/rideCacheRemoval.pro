QT += core gui widgets testlib xml sql network svg concurrent serialport \
      multimedia multimediawidgets webenginecore webenginewidgets webchannel \
      positioning webenginequick charts openglwidgets core5compat

TEMPLATE = app
TARGET = tst_rideCacheRemoval

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testRideCacheRemoval.cpp \
          RideCacheRemovalTestStubs.cpp \
          ../../../src/Core/RideCacheRemoval.cpp

HEADERS = ../../../src/Core/Athlete.h \
          ../../../src/Core/Context.h \
          ../../../src/Core/RideCache.h \
          ../../../src/Core/RideCacheModel.h \
          ../../../src/Core/RideItem.h \
          ../../../src/Metrics/Estimator.h

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
               ../../../qwt/src

QMAKE_CXXFLAGS += -ffunction-sections -fdata-sections
QMAKE_LFLAGS += -Wl,--gc-sections

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
