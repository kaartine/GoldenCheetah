QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameGapJumpIntegration

SOURCES = testWorkoutGameGapJumpIntegration.cpp \
          ../../../src/Train/WorkoutGameGapJumpGeometry.cpp \
          ../../../src/Train/WorkoutGameGapJumpSelector.cpp

HEADERS = ../../../src/Train/WorkoutGameGapJumpGeometry.h \
          ../../../src/Train/WorkoutGameGapJumpSelector.h \
          ../../../src/Train/WorkoutGameWorld.h \
          ../../../src/Train/WorkoutGameCourseDocument.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
