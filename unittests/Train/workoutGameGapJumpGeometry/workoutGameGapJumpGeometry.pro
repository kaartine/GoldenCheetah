QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameGapJumpGeometry

SOURCES = testWorkoutGameGapJumpGeometry.cpp \
          ../../../src/Train/WorkoutGameGapJumpGeometry.cpp

HEADERS = ../../../src/Train/WorkoutGameGapJumpGeometry.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
