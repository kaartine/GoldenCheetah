QT += testlib core gui widgets xml core5compat

SOURCES = testTrainPerspectiveState.cpp \
          ../../../src/Gui/TrainPerspectiveState.cpp \
          ../../../src/Train/RealtimeData.cpp

HEADERS = ../../../src/Gui/TrainPerspectiveState.h \
          ../../../src/Train/RealtimeData.h

INCLUDEPATH += ../../../src \
               ../../../src/ANT \
               ../../../src/Charts \
               ../../../src/Cloud \
               ../../../src/Core \
               ../../../src/FileIO \
               ../../../src/Gui \
               ../../../src/Metrics \
               ../../../src/Planning \
               ../../../src/Train

include(../../unittests.pri)
