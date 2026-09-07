QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameTrainerTargetPlanner

SOURCES = testWorkoutGameTrainerTargetPlanner.cpp \
          ../../../src/Train/TrainerTargetCoordinator.cpp \
          ../../../src/Train/WorkoutGameTrainerTargetPlanner.cpp

HEADERS = ../../../src/Train/TrainerTargetCoordinator.h \
          ../../../src/Train/WorkoutGameCoursePrescription.h \
          ../../../src/Train/WorkoutGameTrainerTargetPlanner.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
