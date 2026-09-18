QT += core testlib

TEMPLATE = app
TARGET = testWorkoutGenerator

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testWorkoutGenerator.cpp \
          ../../../src/Train/WorkoutGenerator.cpp

HEADERS = ../../../src/Train/WorkoutGenerator.h

INCLUDEPATH += ../../../src \
               ../../../src/Train

include(../../section-gc.prf)

sanitize:!tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
