QT += core testlib

TEMPLATE = app
TARGET = testWorkoutFileWriter

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testWorkoutFileWriter.cpp \
          ../../../src/Train/WorkoutFileWriter.cpp

HEADERS = ../../../src/Train/WorkoutFileWriter.h

INCLUDEPATH += ../../../src \
               ../../../src/Train

include(../../section-gc.prf)

sanitize:!tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
