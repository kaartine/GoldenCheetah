QT += core testlib

TEMPLATE = app
TARGET = tst_planReplacementJournal

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug
DEFINES += GC_PLAN_REPLACEMENT_TEST_HOOKS

SOURCES = testPlanReplacementJournal.cpp \
          ../../../src/Planning/PlanReplacementJournal.cpp \
          ../../../src/FileIO/AnchoredFileSystem.cpp

HEADERS = ../../../src/Planning/PlanReplacementJournal.h \
          ../../../src/FileIO/AtomicFileWriter.h \
          ../../../src/FileIO/AnchoredFileSystem.h

INCLUDEPATH += ../../../src/FileIO \
               ../../../src/Planning

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
