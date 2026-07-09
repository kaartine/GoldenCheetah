QT += core gui widgets testlib xml sql network svg concurrent serialport \
      multimedia multimediawidgets webenginecore webenginewidgets webchannel \
      positioning webenginequick

greaterThan(QT_MAJOR_VERSION, 5): QT += core5compat

TEMPLATE = app
TARGET = tst_athleteMigrationSafety

include(../../unittests.pri)

INCLUDEPATH += ../../../contrib/qtsolutions
LIBS += -lz

CONFIG += console testcase c++17 release
CONFIG -= debug
DEFINES += GC_TEST_CLOUD_AUTODOWNLOAD_PROBE

isEmpty(GC_UPGRADE_SOURCE) {
    GC_UPGRADE_SOURCE = ../../../src/Core/GcUpgrade.cpp
}

isEmpty(GC_ATHLETE_SOURCE) {
    GC_ATHLETE_SOURCE = ../../../src/Core/Athlete.cpp
}

isEmpty(GC_CLOUD_SERVICE_SOURCE) {
    GC_CLOUD_SERVICE_SOURCE = ../../../src/Cloud/CloudService.cpp
}

SOURCES = testAthleteMigrationSafety.cpp \
          AthleteMigrationTestStubs.cpp \
          CloudAutoDownloadTestSupport.cpp \
          ../../../src/Cloud/LocalFileStore.cpp \
          ../../../src/Cloud/LocalFileStoreProcess.cpp \
          ../../../src/Cloud/MeasuresDownload.cpp \
          ../../../src/Cloud/NetworkReplyWait.cpp \
          ../../../src/Cloud/NolioTokenRefresh.cpp \
          ../../../src/Cloud/OAuthCallbackPolicy.cpp \
          ../../../src/Cloud/OAuthPKCE.cpp \
          ../../../src/Cloud/SportsPlusHealth.cpp \
          ../../../src/Cloud/TredictMeasuresDownload.cpp \
          ../../../src/Cloud/TrainingsTageBuch.cpp \
          ../../../src/Cloud/WithingsDownload.cpp \
          ../../../contrib/qzip/zip.cpp \
          $$GC_UPGRADE_SOURCE \
          $$GC_ATHLETE_SOURCE \
          $$GC_CLOUD_SERVICE_SOURCE

HEADERS = CloudAutoDownloadTestSupport.h \
          ../../../src/Cloud/CalendarDownload.h \
          ../../../src/Cloud/CloudService.h \
          ../../../src/Cloud/LocalFileStore.h \
          ../../../src/Cloud/LocalFileStoreProcess.h \
          ../../../src/Cloud/MeasuresDownload.h \
          ../../../src/Cloud/NetworkReplyWait.h \
          ../../../src/Cloud/NolioTokenRefresh.h \
          ../../../src/Cloud/OAuthCallbackPolicy.h \
          ../../../src/Cloud/OAuthPKCE.h \
          ../../../src/Cloud/SportsPlusHealth.h \
          ../../../src/Cloud/TredictMeasuresDownload.h \
          ../../../src/Cloud/TrainingsTageBuch.h \
          ../../../src/Cloud/WithingsDownload.h \
          ../../../src/Core/Athlete.h \
          ../../../src/Core/Context.h \
          ../../../src/Core/GcUpgrade.h \
          ../../../src/Core/NamedSearch.h \
          ../../../src/Core/RideCache.h \
          ../../../src/Core/RideItem.h \
          ../../../src/Core/Route.h \
          ../../../src/Core/Seasons.h \
          ../../../src/FileIO/MeasuresCsvImport.h \
          ../../../src/Gui/HelpWhatsThis.h \
          ../../../src/FileIO/RideAutoImportConfig.h \
          ../../../src/Metrics/HrZones.h \
          ../../../src/Metrics/PaceZones.h \
          ../../../src/Metrics/Zones.h \
          ../../../src/Train/TrainDB.h

INCLUDEPATH += ../../../src \
               ../../../src/ANT \
               ../../../src/Charts \
               ../../../src/Core \
               ../../../src/FileIO \
               ../../../src/Gui \
               ../../../src/Metrics \
               ../../../src/Planning \
               ../../../src/Train \
               ../../../src/Cloud \
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
