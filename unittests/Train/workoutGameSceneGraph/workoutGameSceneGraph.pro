QT += core gui widgets quick testlib
CONFIG += c++17

TARGET = testWorkoutGameSceneGraph

SOURCES = testWorkoutGameSceneGraph.cpp \
          ../../../src/Train/WorkoutGameFeatureChallenge.cpp \
          ../../../src/Train/WorkoutGameFeatureLab.cpp \
          ../../../src/Train/WorkoutGameFeatureRuntime.cpp \
          ../../../src/Train/WorkoutGameRoadCourse.cpp \
          ../../../src/Train/WorkoutGameRoadProjection.cpp \
          ../../../src/Train/WorkoutGameSceneGraphWindow.cpp \
          ../../../src/Train/WorkoutGameTerrainTransition.cpp \
          ../../../src/Train/WorkoutGameVisualSmoother.cpp

HEADERS = ../../../src/Train/WorkoutGameFeatureChallenge.h \
          ../../../src/Train/WorkoutGameFeatureLab.h \
          ../../../src/Train/WorkoutGameFeatureRuntime.h \
          ../../../src/Train/WorkoutGameRoadCourse.h \
          ../../../src/Train/WorkoutGameRoadProjection.h \
          ../../../src/Train/WorkoutGameSceneGraphWindow.h \
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
