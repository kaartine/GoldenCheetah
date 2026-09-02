QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameGapJumpLaunchWindow

SOURCES = testWorkoutGameGapJumpLaunchWindow.cpp \
          ../../../src/Train/WorkoutGameGapJumpLaunchWindow.cpp

HEADERS = ../../../src/Train/WorkoutGameGapJumpLaunchWindow.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
