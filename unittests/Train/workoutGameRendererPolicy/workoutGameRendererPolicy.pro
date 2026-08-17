QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameRendererPolicy

SOURCES = testWorkoutGameRendererPolicy.cpp \
          ../../../src/Train/WorkoutGameRendererPolicy.cpp

HEADERS = ../../../src/Train/WorkoutGameRendererPolicy.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
