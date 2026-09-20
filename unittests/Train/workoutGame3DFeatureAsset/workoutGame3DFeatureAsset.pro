QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGame3DFeatureAsset

SOURCES = testWorkoutGame3DFeatureAsset.cpp \
          ../../../src/Train/WorkoutGame3DFeatureAsset.cpp \
          ../../../src/Train/WorkoutGameAssetPhysicsSampler.cpp \
          ../../../src/Train/WorkoutGameRoadCourse.cpp \
          ../../../src/Train/WorkoutGameAssetPhysicsSnapshot.cpp \
          ../../../src/Train/WorkoutGameRoadPlan.cpp \
          ../../../src/Train/WorkoutGameRoadQuality.cpp \
          ../../../src/Train/WorkoutGameFeatureChallenge.cpp \
          ../../../src/Train/WorkoutGameGapJumpGeometry.cpp \
          ../../../src/Train/WorkoutGameMesh.cpp \
          ../../../src/Train/WorkoutGameRoadProjection.cpp

HEADERS = ../../../src/Train/WorkoutGame3DFeatureAsset.h \
          ../../../src/Train/WorkoutGameAssetPhysicsSampler.h \
          ../../../src/Train/WorkoutGameRoadCourse.h \
          ../../../src/Train/WorkoutGameFeatureGeometry.h \
          ../../../src/Train/WorkoutGameGapJumpGeometry.h \
          ../../../src/Train/WorkoutGameTabletopGeometry.h \
          ../../../src/Train/WorkoutGameMesh.h \
          ../../../src/Train/WorkoutGameRoadProjection.h

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
