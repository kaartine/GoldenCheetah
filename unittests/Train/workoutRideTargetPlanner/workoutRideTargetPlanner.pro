QT += core testlib
CONFIG += c++17

TARGET = testWorkoutRideTargetPlanner

SOURCES = testWorkoutRideTargetPlanner.cpp \
          ../../../src/Train/VirtualDrivetrain.cpp \
          ../../../src/Train/WorkoutRideTargetPlanner.cpp

HEADERS = ../../../src/Train/VirtualDrivetrain.h \
          ../../../src/Train/WorkoutRideTargetPlanner.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
