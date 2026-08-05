QT += core gui widgets sql testlib

TEMPLATE = app
TARGET = tst_libraryTransactionSafety

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

DEFINES += GC_LIBRARY_TRANSACTION_TEST_HOOKS

SOURCES = testLibraryTransactionSafety.cpp \
          LibraryTransactionTestStubs.cpp \
          ../../../src/Train/Library.cpp \
          ../../../src/Train/LibraryImportFileStager.cpp \
          ../../../src/Train/WorkoutImportBatch.cpp \
          ../../../src/Train/TrainDB.cpp \
          ../../../src/Train/ErgFileBase.cpp \
          ../../../src/Train/VideoSyncFileBase.cpp

HEADERS = LibraryTransactionTestStubs.h \
          ../../../src/Train/LibraryImportFileStager.h \
          ../../../src/Train/WorkoutImportBatch.h \
          ../../../src/Train/TrainDB.h \
          ../../../src/Train/ErgFileBase.h \
          ../../../src/Train/VideoSyncFileBase.h \
          ../../../src/Train/TagStore.h

INCLUDEPATH += . \
               ../../../src \
               ../../../src/Charts \
               ../../../src/Core \
               ../../../src/FileIO \
               ../../../src/Gui \
               ../../../src/Train

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
