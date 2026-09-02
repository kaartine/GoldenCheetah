QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameGapJumpSelector

SOURCES = testWorkoutGameGapJumpSelector.cpp \
          ../../../src/Train/WorkoutGameGapJumpGeometry.cpp \
          ../../../src/Train/WorkoutGameGapJumpSelector.cpp

HEADERS = ../../../src/Train/WorkoutGameGapJumpGeometry.h \
          ../../../src/Train/WorkoutGameGapJumpSelector.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
