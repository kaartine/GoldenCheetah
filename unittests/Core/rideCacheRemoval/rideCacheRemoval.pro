QT += core gui widgets testlib xml sql network svg concurrent serialport \
      multimedia multimediawidgets webenginecore webenginewidgets webchannel \
      positioning webenginequick charts openglwidgets core5compat

TEMPLATE = app
TARGET = tst_rideCacheRemoval

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug
DEFINES += GC_RIDE_CACHE_REMOVAL_TEST_HOOKS

SOURCES = testRideCacheRemoval.cpp \
          RideCacheRemovalTestStubs.cpp \
          ../../../src/FileIO/RideFileCRC.cpp \
          ../../../src/FileIO/RideFileCacheIntegrity.cpp \
          ../../../src/Core/LinkedActivityRemovalJournal.cpp \
          ../../../src/Core/LinkedActivitySaveJournal.cpp \
          ../../../src/Planning/PlanReplacementJournal.cpp \
          ../../../src/Core/PlannedActivityFileStager.cpp \
          ../../../src/Core/RideCacheActivityLinking.cpp \
          ../../../src/Core/RideCacheGarbageCollection.cpp \
          ../../../src/Core/RideCacheImport.cpp \
          ../../../src/Core/RideCacheLiveView.cpp \
          ../../../src/Core/RideCacheMutationScope.cpp \
          ../../../src/Core/RideCacheRemoval.cpp \
          ../../../src/Core/RideCacheModelProtocol.cpp

HEADERS = ../../../src/Core/Athlete.h \
          ../../../src/Core/Context.h \
          ../../../src/Core/RideCache.h \
          ../../../src/Core/RideCacheMutationScope.h \
          ../../../src/Core/RideCacheModel.h \
          ../../../src/Core/RideItem.h \
          ../../../src/Core/LinkedActivityRemovalJournal.h \
          ../../../src/Core/LinkedActivitySaveJournal.h \
          ../../../src/Planning/PlanReplacementJournal.h \
          ../../../src/FileIO/RideFile.h \
          ../../../src/FileIO/RideFileCRC.h \
          ../../../src/FileIO/RideFileCacheIntegrity.h \
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

sanitize:!tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}

tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=thread \
                      -fno-omit-frame-pointer \
                      -fno-pie \
                      -O1 \
                      -g
    QMAKE_LFLAGS += -fsanitize=thread \
                    -no-pie
}
