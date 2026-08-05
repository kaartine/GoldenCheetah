QT += core gui widgets testlib xml sql network svg concurrent serialport \
      multimedia multimediawidgets webenginecore webenginewidgets webchannel \
      positioning webenginequick charts openglwidgets core5compat

TEMPLATE = app
TARGET = tst_rideCacheRemoval

CONFIG += console testcase no_testcase_installs c++17 release
CONFIG -= debug
DEFINES += GC_RIDE_CACHE_REMOVAL_TEST_HOOKS \
           GC_ANCHORED_FILESYSTEM_TEST_HOOKS \
           GC_PLAN_REPLACEMENT_TEST_HOOKS

SOURCES = testRideCacheRemoval.cpp \
          RideCacheRemovalTestStubs.cpp \
          ../../../src/Core/AthleteSession.cpp \
          ../../../src/Core/TrainingSession.cpp \
          ../../../src/FileIO/AnchoredFileSystem.cpp \
          ../../../src/FileIO/LocationInterpolation.cpp \
          ../../../src/FileIO/RideFileCRC.cpp \
          ../../../src/FileIO/RideFileCacheIntegrity.cpp \
          ../../../src/Metrics/BlinnSolver.cpp \
          ../../../src/Core/LinkedActivityRemovalJournal.cpp \
          ../../../src/Core/LinkedActivitySaveJournal.cpp \
          ../../../src/Planning/PlanBundle.cpp \
          ../../../src/Planning/PlanBundleImportJournal.cpp \
          ../../../src/Planning/PlanReplacementJournal.cpp \
          ../../../src/Train/TrainDB.cpp \
          ../../../src/Train/ErgFileBase.cpp \
          ../../../src/Train/VideoSyncFileBase.cpp \
          ../../../src/Core/PlannedActivityFileStager.cpp \
          ../../../src/Core/RideCacheActivityLinking.cpp \
          ../../../src/Core/RideCacheCalendarMutations.cpp \
          ../../../src/Core/RideCacheGarbageCollection.cpp \
          ../../../src/Core/RideCacheImport.cpp \
          ../../../src/Core/RideCacheLiveView.cpp \
          ../../../src/Core/RideCacheMutationScope.cpp \
          ../../../src/Core/RideCacheRemoval.cpp \
          ../../../src/Core/RideCacheModelProtocol.cpp \
          ../../../contrib/qzip/zip.cpp

HEADERS = ../../../src/Core/Athlete.h \
          ../../../src/Core/AthleteSession.h \
          ../../../src/Core/TrainingSession.h \
          ../../../src/Core/SessionServices.h \
          ../../../src/FileIO/AnchoredFileSystem.h \
          ../../../src/Core/Context.h \
          ../../../src/Core/RideCache.h \
          ../../../src/Core/RideCacheMutationScope.h \
          ../../../src/Core/RideCacheModel.h \
          ../../../src/Core/RideItem.h \
          ../../../src/Core/LinkedActivityRemovalJournal.h \
          ../../../src/Core/LinkedActivitySaveJournal.h \
          ../../../src/Planning/PlanBundle.h \
          ../../../src/Planning/PlanBundleImportJournal.h \
          ../../../src/Planning/PlanReplacementJournal.h \
          ../../../src/Train/TrainDB.h \
          ../../../src/FileIO/LocationInterpolation.h \
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
               ../../../qwt/src \
               ../../../contrib/qzip \
               $${GSL_INCLUDES}

LIBS += $${GSL_LIBS}
win32:LIBS += -ladvapi32

unix {
    LIBS += -lz
}

include(../../section-gc.prf)

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
