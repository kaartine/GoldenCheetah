QT += core gui widgets testlib xml sql network svg concurrent serialport \
      multimedia multimediawidgets webenginecore webenginewidgets webchannel \
      positioning webenginequick

greaterThan(QT_MAJOR_VERSION, 5): QT += core5compat

TEMPLATE = app
TARGET = tst_athleteMigrationSafety

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

isEmpty(GC_UPGRADE_SOURCE) {
    GC_UPGRADE_SOURCE = ../../../src/Core/GcUpgrade.cpp
}

isEmpty(GC_ATHLETE_SOURCE) {
    GC_ATHLETE_SOURCE = ../../../src/Core/Athlete.cpp
}

SOURCES = testAthleteMigrationSafety.cpp \
          AthleteMigrationTestStubs.cpp \
          $$GC_UPGRADE_SOURCE \
          $$GC_ATHLETE_SOURCE

HEADERS = ../../../src/Cloud/CalendarDownload.h \
          ../../../src/Cloud/CloudService.h \
          ../../../src/Core/Athlete.h \
          ../../../src/Core/Context.h \
          ../../../src/Core/GcUpgrade.h \
          ../../../src/Core/NamedSearch.h \
          ../../../src/Core/RideCache.h \
          ../../../src/Core/RideItem.h \
          ../../../src/Core/Route.h \
          ../../../src/Core/Seasons.h \
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

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
