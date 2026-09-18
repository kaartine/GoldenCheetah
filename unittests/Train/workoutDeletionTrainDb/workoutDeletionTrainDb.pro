QT += core gui widgets sql testlib

TEMPLATE = app
TARGET = testWorkoutDeletionTrainDb

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

DEFINES += GC_TRAIN_DB_TEST_HOOKS

SOURCES = testWorkoutDeletionTrainDb.cpp \
          ../../../src/FileIO/AnchoredFileSystem.cpp \
          ../../../src/Train/WorkoutDeletionService.cpp \
          ../../../src/Train/WorkoutDeletionServiceTrainDB.cpp \
          ../../../src/Train/TrainDB.cpp \
          ../../../src/Train/ErgFileBase.cpp \
          ../../../src/Train/VideoSyncFileBase.cpp

HEADERS = ../../../src/FileIO/AnchoredFileSystem.h \
          ../../../src/Train/WorkoutDeletionService.h \
          ../../../src/Train/TrainDB.h \
          ../../../src/Train/ErgFileBase.h \
          ../../../src/Train/VideoSyncFileBase.h \
          ../../../src/Train/TagStore.h

INCLUDEPATH += ../../../src \
               ../../../src/Charts \
               ../../../src/Core \
               ../../../src/FileIO \
               ../../../src/Gui \
               ../../../src/Train

win32:LIBS += -ladvapi32

include(../../section-gc.prf)

sanitize:!tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
