QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameClock

SOURCES = testWorkoutGameClock.cpp \
          ../../../src/Train/WorkoutGameClock.cpp

HEADERS = ../../../src/Train/WorkoutGameClock.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
