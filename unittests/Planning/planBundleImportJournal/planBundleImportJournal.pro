QT += core gui widgets sql testlib

TEMPLATE = app
TARGET = tst_planBundleImportJournal

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug
DEFINES += GC_PLAN_BUNDLE_IMPORT_TEST_HOOKS \
           GC_ANCHORED_FILESYSTEM_TEST_HOOKS \
           GC_TRAIN_DB_TEST_HOOKS \
           GC_TRAIN_DB_PERSISTENCE_TEST_HOOKS

SOURCES = testPlanBundleImportJournal.cpp \
          ../../../src/Planning/PlanBundleImportJournal.cpp \
          ../../../src/Planning/PlanReplacementJournal.cpp \
          ../../../src/FileIO/AnchoredFileSystem.cpp \
          ../../../src/Train/TrainDB.cpp \
          ../../../src/Train/ErgFileBase.cpp \
          ../../../src/Train/VideoSyncFileBase.cpp

HEADERS = ../../../src/Planning/PlanBundleImportJournal.h \
          ../../../src/Planning/PlanReplacementJournal.h \
          ../../../src/FileIO/AtomicFileWriter.h \
          ../../../src/FileIO/AnchoredFileSystem.h \
          ../../../src/Train/TrainDB.h

INCLUDEPATH += ../../../src \
               ../../../src/Charts \
               ../../../src/Core \
               ../../../src/FileIO \
               ../../../src/Gui \
               ../../../src/Planning \
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
