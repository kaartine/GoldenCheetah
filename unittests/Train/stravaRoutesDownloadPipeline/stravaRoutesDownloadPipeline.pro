QT += concurrent core core5compat network sql testlib widgets xml

TEMPLATE = app
TARGET = testStravaRoutesDownloadPipeline

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug
DEFINES += GC_STRAVA_ROUTES_PIPELINE_TEST_HOOKS \
           GC_PLAN_BUNDLE_IMPORT_TEST_HOOKS \
           GC_ERG_FILE_GPX_TEST_HOOKS

SOURCES = testStravaRoutesDownloadPipeline.cpp \
          ErgFileGpxCompositionTestStubs.cpp \
          ../../../src/Cloud/StravaTokenRefresh.cpp \
          ../../../src/FileIO/AnchoredFileSystem.cpp \
          ../../../src/FileIO/GpxParser.cpp \
          ../../../src/FileIO/LocationInterpolation.cpp \
          ../../../src/FileIO/RideFile.cpp \
          ../../../src/FileIO/RideFileCommand.cpp \
          ../../../src/FileIO/RideFileCRC.cpp \
          ../../../src/Metrics/BlinnSolver.cpp \
          ../../../src/Core/TimeUtils.cpp \
          ../../../src/Train/ErgFile.cpp \
          ../../../src/Train/ErgFileBase.cpp \
          ../../../src/Train/ErgFileBytes.cpp \
          ../../../src/Train/StravaRoutesClient.cpp \
          ../../../src/Train/StravaRoutesDownload.cpp \
          ../../../src/Train/StravaRoutesDownloadPipeline.cpp \
          ../../../src/Train/TrainDB.cpp \
          ../../../src/Planning/PlanBundleImportJournal.cpp \
          ../../../src/Planning/PlanReplacementJournal.cpp \
          ../../../src/Train/VideoSyncFileBase.cpp

HEADERS = ../../../src/Cloud/StravaTokenRefresh.h \
          ../../../src/Train/ErgFile.h \
          ../../../src/Train/ErgFileBase.h \
          ../../../src/Train/StravaRoutesClient.h \
          ../../../src/Train/StravaRoutesDownload.h \
          ../../../src/Train/StravaRoutesDownloadPipeline.h \
          ../../../src/Train/TrainDB.h \
          ../../../src/Planning/PlanBundleImportJournal.h \
          ../../../src/Planning/PlanReplacementJournal.h \
          ../../../src/Train/VideoSyncFileBase.h \
          ../../../src/FileIO/AnchoredFileSystem.h \
          ../../../src/FileIO/RideFile.h \
          ../../../src/FileIO/RideFileCommand.h \
          ../../../src/Cloud/StravaAuthenticatedSession.h \
          ../../../src/Cloud/StravaNetworkReply.h

INCLUDEPATH += ../../../src \
               ../../../src/ANT \
               ../../../src/Charts \
               ../../../src/Cloud \
               ../../../src/Core \
               ../../../src/FileIO \
               ../../../src/Gui \
               ../../../src/Metrics \
               ../../../src/Train \
               ../../../qwt/src

win32:LIBS += -ladvapi32

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
                      -O1 \
                      -g
    QMAKE_LFLAGS += -fsanitize=thread
    linux {
        QMAKE_CXXFLAGS += -fno-pie
        QMAKE_LFLAGS += -no-pie
    }
}

include(../../section-gc.prf)
