QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameFeatureChallenge

SOURCES = testWorkoutGameFeatureChallenge.cpp \
          ../../../src/Train/WorkoutGameFeatureChallenge.cpp

HEADERS = ../../../src/Train/WorkoutGameCourse.h \
          ../../../src/Train/WorkoutGameFeatureChallenge.h \
          ../../../src/Train/WorkoutGameWorld.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
