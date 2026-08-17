QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameCompetition

SOURCES = testWorkoutGameCompetition.cpp \
          ../../../src/Train/WorkoutGameCompetition.cpp \
          ../../../src/Train/WorkoutGameCourse.cpp

HEADERS = ../../../src/Train/WorkoutGameCompetition.h \
          ../../../src/Train/WorkoutGameCourse.h \
          ../../../src/Train/WorkoutGameSimulation.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
