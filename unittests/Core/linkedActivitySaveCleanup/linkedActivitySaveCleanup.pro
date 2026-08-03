QT += core testlib
QT -= gui

TEMPLATE = app
TARGET = tst_linkedActivitySaveCleanup

CONFIG += console testcase no_testcase_installs c++17 release
CONFIG -= debug
DEFINES += GC_ANCHORED_FILESYSTEM_TEST_HOOKS

SOURCES = testLinkedActivitySaveCleanup.cpp \
          ../../../src/Core/LinkedActivitySaveJournal.cpp \
          ../../../src/FileIO/AnchoredFileSystem.cpp

HEADERS = ../../../src/Core/LinkedActivitySaveJournal.h \
          ../../../src/FileIO/AnchoredFileSystem.h \
          ../../../src/FileIO/AtomicFileWriter.h

INCLUDEPATH += ../../../src/Core \
               ../../../src/FileIO
