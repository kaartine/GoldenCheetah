QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameFeatureHud

SOURCES = testWorkoutGameFeatureHud.cpp \
          ../../../src/Train/WorkoutGameFeatureHud.cpp

HEADERS = ../../../src/Train/WorkoutGameFeatureHud.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
