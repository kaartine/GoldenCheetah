QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameWorkoutAdapter

SOURCES = testWorkoutGameWorkoutAdapter.cpp \
          ../../../src/Train/WorkoutGameWorkoutAdapter.cpp

HEADERS = ../../../src/Train/WorkoutGameCourse.h \
          ../../../src/Train/WorkoutGameWorkoutAdapter.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
