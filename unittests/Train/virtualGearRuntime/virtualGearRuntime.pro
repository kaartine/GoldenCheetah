QT += core gui widgets testlib core5compat
CONFIG += c++17

TARGET = testVirtualGearRuntime

SOURCES = testVirtualGearRuntime.cpp \
          ../../../src/Train/TrainingCommandRouter.cpp \
          ../../../src/Train/TrainingCsvSeries.cpp \
          ../../../src/Train/RealtimeData.cpp

HEADERS = ../../../src/Train/TrainingCommandRouter.h \
          ../../../src/Train/TrainingCsvSeries.h \
          ../../../src/Train/RealtimeData.h

INCLUDEPATH += ../../../src/Charts \
               ../../../src/Core \
               ../../../src/FileIO \
               ../../../src/Gui \
               ../../../src/Metrics \
               ../../../src/Planning \
               ../../../src/Train \
               ../../../qwt/src

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
