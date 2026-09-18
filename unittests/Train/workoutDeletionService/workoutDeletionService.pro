QT += core testlib

TEMPLATE = app
TARGET = testWorkoutDeletionService

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testWorkoutDeletionService.cpp \
          ../../../src/Train/WorkoutDeletionService.cpp

HEADERS = ../../../src/Train/WorkoutDeletionService.h

INCLUDEPATH += ../../../src \
               ../../../src/Train

include(../../section-gc.prf)

sanitize:!tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
