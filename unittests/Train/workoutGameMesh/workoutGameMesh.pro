QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameMesh

SOURCES = testWorkoutGameMesh.cpp \
          ../../../src/Train/WorkoutGameFeatureChallenge.cpp \
          ../../../src/Train/WorkoutGameMesh.cpp \
          ../../../src/Train/WorkoutGameTrailTile.cpp \
          ../../../src/Train/WorkoutGameRoadCourse.cpp \
          ../../../src/Train/WorkoutGameRoadProjection.cpp

HEADERS = ../../../src/Train/WorkoutGameMesh.h \
          ../../../src/Train/WorkoutGameClimbGeometry.h \
          ../../../src/Train/WorkoutGameTabletopGeometry.h \
          ../../../src/Train/WorkoutGameForestFloor.h \
          ../../../src/Train/WorkoutGameOcclusion.h \
          ../../../src/Train/WorkoutGameRootGeometry.h \
          ../../../src/Train/WorkoutGameRockGardenGeometry.h \
          ../../../src/Train/WorkoutGameRockSlabGeometry.h \
          ../../../src/Train/WorkoutGameSkinnyGeometry.h \
          ../../../src/Train/WorkoutGameTrailBranch.h \
          ../../../src/Train/WorkoutGameTrailTile.h \
          ../../../src/Train/WorkoutGameRoadCourse.h \
          ../../../src/Train/WorkoutGameRoadProjection.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
