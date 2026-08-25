QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGame3DTerrainProfile

SOURCES = testWorkoutGame3DTerrainProfile.cpp \
          ../../../src/Train/WorkoutGame3DTerrainProfile.cpp

HEADERS = ../../../src/Train/WorkoutGame3DTerrainProfile.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
