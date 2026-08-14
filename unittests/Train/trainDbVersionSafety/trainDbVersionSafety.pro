QT += core gui widgets sql testlib

TEMPLATE = app
TARGET = tst_trainDbVersionSafety

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

DEFINES += GC_TRAIN_DB_TEST_HOOKS

SOURCES = testTrainDbVersionSafety.cpp \
          ../../../src/FileIO/AnchoredFileSystem.cpp \
          ../../../src/Train/TrainDB.cpp \
          ../../../src/Train/ErgFileBase.cpp \
          ../../../src/Train/VideoSyncFileBase.cpp

HEADERS = ../../../src/Train/TrainDB.h \
          ../../../src/FileIO/AnchoredFileSystem.h \
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

sanitize:!tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
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
