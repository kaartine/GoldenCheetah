QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameEngine

SOURCES = testWorkoutGameEngine.cpp \
          ../../../src/Train/WorkoutGame3DTerrainProfile.cpp \
          ../../../src/Train/WorkoutGameCompetition.cpp \
          ../../../src/Train/WorkoutGameCourse.cpp \
          ../../../src/Train/WorkoutGameEngine.cpp \
          ../../../src/Train/WorkoutGameFeatureChallenge.cpp \
          ../../../src/Train/WorkoutGameFeatureLab.cpp \
          ../../../src/Train/WorkoutGameFeatureRuntime.cpp \
          ../../../src/Train/WorkoutGameRiderVisual.cpp \
          ../../../src/Train/WorkoutGameRoadCourse.cpp \
          ../../../src/Train/WorkoutGameSimulation.cpp \
          ../../../src/Train/WorkoutGameTerrainTransition.cpp \
          ../../../src/Train/WorkoutGameVisualSmoother.cpp \
          ../../../src/Train/WorkoutGameWorld.cpp

HEADERS = ../../../src/Train/WorkoutGame3DTerrainProfile.h \
          ../../../src/Train/WorkoutGameEngine.h \
          ../../../src/Train/WorkoutGameRiderVisual.h

BOX2D_ROOT = $$clean_path($$_PRO_FILE_PWD_/../../../vendor/box2d-3.1.1)
include($$BOX2D_ROOT/box2d.pri)

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CFLAGS += -fsanitize=address,undefined \
                    -fno-omit-frame-pointer \
                    -fno-sanitize-recover=all
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
