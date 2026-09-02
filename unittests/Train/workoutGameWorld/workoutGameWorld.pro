QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameWorld

SOURCES = testWorkoutGameWorld.cpp \
          ../../../src/Train/WorkoutGame3DTerrainProfile.cpp \
          ../../../src/Train/WorkoutGameFeatureChallenge.cpp \
          ../../../src/Train/WorkoutGameGapJumpGeometry.cpp \
          ../../../src/Train/WorkoutGameRoadCourse.cpp \
          ../../../src/Train/WorkoutGameWorld.cpp

HEADERS = ../../../src/Train/WorkoutGameCourse.h \
          ../../../src/Train/WorkoutGame3DTerrainProfile.h \
          ../../../src/Train/WorkoutGameClimbGeometry.h \
          ../../../src/Train/WorkoutGameTabletopGeometry.h \
          ../../../src/Train/WorkoutGameTrailBranch.h \
          ../../../src/Train/WorkoutGameFeatureCatalog.h \
          ../../../src/Train/WorkoutGameFeatureChallenge.h \
          ../../../src/Train/WorkoutGameGapJumpGeometry.h \
          ../../../src/Train/WorkoutGameRoadCourse.h \
          ../../../src/Train/WorkoutGameRootGeometry.h \
          ../../../src/Train/WorkoutGameRockGardenGeometry.h \
          ../../../src/Train/WorkoutGameRockSlabGeometry.h \
          ../../../src/Train/WorkoutGameSkinnyGeometry.h \
          ../../../src/Train/WorkoutGameWorld.h

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
