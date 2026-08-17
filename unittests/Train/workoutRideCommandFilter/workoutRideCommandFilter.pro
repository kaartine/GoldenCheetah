QT += core testlib
CONFIG += c++17

TARGET = testWorkoutRideCommandFilter

SOURCES = testWorkoutRideCommandFilter.cpp \
          ../../../src/Train/WorkoutRideCommandFilter.cpp

HEADERS = ../../../src/Train/WorkoutRideCommandFilter.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
