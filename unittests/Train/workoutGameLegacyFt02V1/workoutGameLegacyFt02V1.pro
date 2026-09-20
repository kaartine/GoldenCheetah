QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameLegacyFt02V1
SOURCES = testWorkoutGameLegacyFt02V1.cpp
HEADERS = ../../../src/Train/WorkoutGameFeatureGeometry.h \
          ../../../src/Train/WorkoutGameLegacyFt02V1.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
