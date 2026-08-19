QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameDistancePlayback

SOURCES = testWorkoutGameDistancePlayback.cpp \
          ../../../src/Train/WorkoutGameCourse.cpp \
          ../../../src/Train/WorkoutGameDistanceCourse.cpp \
          ../../../src/Train/WorkoutGameDistancePlayback.cpp \
          ../../../src/Train/WorkoutGameRoadPhysics.cpp

HEADERS = ../../../src/Train/WorkoutGameCourse.h \
          ../../../src/Train/WorkoutGameDistanceCourse.h \
          ../../../src/Train/WorkoutGameDistancePlayback.h \
          ../../../src/Train/WorkoutGameRoadPhysics.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
