QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameFeatureRuntime

SOURCES = testWorkoutGameFeatureRuntime.cpp \
          ../../../src/Train/WorkoutGameCourse.cpp \
          ../../../src/Train/WorkoutGameFeatureChallenge.cpp \
          ../../../src/Train/WorkoutGameFeatureLab.cpp \
          ../../../src/Train/WorkoutGameFeatureRuntime.cpp \
          ../../../src/Train/WorkoutGameGapJumpGeometry.cpp \
          ../../../src/Train/WorkoutGameGapJumpLaunchWindow.cpp \
          ../../../src/Train/WorkoutGameGapJumpSelector.cpp \
          ../../../src/Train/WorkoutGameRoadCourse.cpp \
          ../../../src/Train/WorkoutGameSimulation.cpp

HEADERS = ../../../src/Train/WorkoutGameCourse.h \
          ../../../src/Train/WorkoutGameClimbGeometry.h \
          ../../../src/Train/WorkoutGameTabletopGeometry.h \
          ../../../src/Train/WorkoutGameFeatureChallenge.h \
          ../../../src/Train/WorkoutGameFeatureLab.h \
          ../../../src/Train/WorkoutGameFeatureRuntime.h \
          ../../../src/Train/WorkoutGameGapJumpGeometry.h \
          ../../../src/Train/WorkoutGameGapJumpLaunchWindow.h \
          ../../../src/Train/WorkoutGameGapJumpSelector.h \
          ../../../src/Train/WorkoutGameRoadCourse.h \
          ../../../src/Train/WorkoutGameRootGeometry.h \
          ../../../src/Train/WorkoutGameRockGardenGeometry.h \
          ../../../src/Train/WorkoutGameRockSlabGeometry.h \
          ../../../src/Train/WorkoutGameSkinnyGeometry.h \
          ../../../src/Train/WorkoutGameSimulation.h \
          ../../../src/Train/WorkoutGameWorld.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
