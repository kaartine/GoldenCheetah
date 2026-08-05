QT += core gui widgets testlib xml sql network svg concurrent serialport \
      multimedia multimediawidgets webenginecore webenginewidgets webchannel \
      positioning webenginequick charts openglwidgets core5compat

TEMPLATE = app
TARGET = tst_rideFileCacheRefresh

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug
DEFINES += GC_RIDE_FILE_CACHE_TEST_HOOKS \
           GC_RIDE_FILE_SOURCE_PROVENANCE_TEST_HOOKS

SOURCES = testRideFileCacheRefresh.cpp \
          RideFileCacheRefreshTestStubs.cpp \
          ../../../src/FileIO/RideFile.cpp \
          ../../../src/FileIO/RideFileCRC.cpp \
          ../../../src/FileIO/RideFileCommand.cpp \
          ../../../src/FileIO/RideFileCache.cpp \
          ../../../src/FileIO/RideFileCacheIntegrity.cpp \
          ../../../src/FileIO/RideFileCacheWriteError.cpp

HEADERS = ../../../src/FileIO/RideFile.h \
          ../../../src/FileIO/RideFileCommand.h \
          ../../../src/FileIO/RideFileCache.h \
          ../../../src/FileIO/RideFileCacheIntegrity.h \
          ../../../src/FileIO/RideFileCacheWriteError.h \
          ../../../src/Core/SessionServices.h

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

sanitize:!tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}

tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=thread -fno-omit-frame-pointer
    QMAKE_LFLAGS += -fsanitize=thread
}
