QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameRoadPlan

SOURCES = testWorkoutGameRoadPlan.cpp \
          ../../../src/Train/WorkoutGameRoadPlan.cpp \
          ../../../src/Train/WorkoutGameRoadQuality.cpp

HEADERS = ../../../src/Train/WorkoutGameRoadPlan.h \
          ../../../src/Train/WorkoutGameRoadQuality.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
