QT += core gui widgets sql testlib

TEMPLATE = app
TARGET = tst_trainDbVersionSafety

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testTrainDbVersionSafety.cpp \
          ../../../src/Train/TrainDB.cpp \
          ../../../src/Train/ErgFileBase.cpp \
          ../../../src/Train/VideoSyncFileBase.cpp

HEADERS = ../../../src/Train/TrainDB.h \
          ../../../src/Train/ErgFileBase.h \
          ../../../src/Train/VideoSyncFileBase.h \
          ../../../src/Train/TagStore.h

INCLUDEPATH += ../../../src \
               ../../../src/Charts \
               ../../../src/Core \
               ../../../src/Gui \
               ../../../src/Train

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
