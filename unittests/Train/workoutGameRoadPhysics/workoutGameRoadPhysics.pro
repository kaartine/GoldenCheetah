QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameRoadPhysics

SOURCES = testWorkoutGameRoadPhysics.cpp \
          ../../../src/Train/WorkoutGameRoadPhysics.cpp

HEADERS = ../../../src/Train/WorkoutGameRoadPhysics.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
