QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameCourse

SOURCES = testWorkoutGameCourse.cpp \
          ../../../src/Train/WorkoutGameCourse.cpp

HEADERS = ../../../src/Train/WorkoutGameCourse.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
