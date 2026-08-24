QT += core gui widgets quick testlib
CONFIG += c++17

TARGET = testWorkoutGameSceneGraph

SOURCES = testWorkoutGameSceneGraph.cpp \
          ../../../src/Train/WorkoutGameClock.cpp \
          ../../../src/Train/WorkoutGameDiagnostics.cpp \
          ../../../src/Train/WorkoutGameFeatureChallenge.cpp \
          ../../../src/Train/WorkoutGameFeatureLab.cpp \
          ../../../src/Train/WorkoutGameFeaturePrompt.cpp \
          ../../../src/Train/WorkoutGameFeatureRuntime.cpp \
          ../../../src/Train/WorkoutGameHorizon.cpp \
          ../../../src/Train/WorkoutGameMesh.cpp \
          ../../../src/Train/WorkoutGameTrailTile.cpp \
          ../../../src/Train/WorkoutGamePowerCueGeometry.cpp \
          ../../../src/Train/WorkoutGamePowerProfile.cpp \
          ../../../src/Train/WorkoutGameRiderVisual.cpp \
          ../../../src/Train/WorkoutGameRoadCourse.cpp \
          ../../../src/Train/WorkoutGameRoadProjection.cpp \
          ../../../src/Train/WorkoutGameSceneGraphWindow.cpp \
          ../../../src/Train/WorkoutGameSimulation.cpp \
          ../../../src/Train/WorkoutGameTerrainTransition.cpp \
          ../../../src/Train/WorkoutGameVisualSmoother.cpp

HEADERS = ../../../src/Train/WorkoutGameFeatureChallenge.h \
          ../../../src/Train/WorkoutGameClock.h \
          ../../../src/Train/WorkoutGameDiagnostics.h \
          ../../../src/Train/WorkoutGameFeatureLab.h \
          ../../../src/Train/WorkoutGameFeaturePrompt.h \
          ../../../src/Train/WorkoutGameFeatureRuntime.h \
          ../../../src/Train/WorkoutGameHorizon.h \
          ../../../src/Train/WorkoutGameMesh.h \
          ../../../src/Train/WorkoutGameForestFloor.h \
          ../../../src/Train/WorkoutGameTrailBranch.h \
          ../../../src/Train/WorkoutGameTrailTile.h \
          ../../../src/Train/WorkoutGamePowerCueGeometry.h \
          ../../../src/Train/WorkoutGamePowerProfile.h \
          ../../../src/Train/WorkoutGameRiderVisual.h \
          ../../../src/Train/WorkoutGameRoadCourse.h \
          ../../../src/Train/WorkoutGameRoadProjection.h \
          ../../../src/Train/WorkoutGameSceneGraphWindow.h \
          ../../../src/Train/WorkoutGameSimulation.h \
          ../../../src/Train/WorkoutGameTerrainTransition.h \
          ../../../src/Train/WorkoutGameVisualSmoother.h

RESOURCES = workoutGameSceneGraph.qrc

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
