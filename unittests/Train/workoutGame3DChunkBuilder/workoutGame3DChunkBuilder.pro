QT += core gui quick3d testlib
CONFIG += testcase console c++17
TEMPLATE = app
TARGET = testWorkoutGame3DChunkBuilder

INCLUDEPATH += ../../../src/Train

SOURCES += testWorkoutGame3DChunkBuilder.cpp \
           ../../../src/Train/WorkoutGame3DChunkBuilder.cpp \
           ../../../src/Train/WorkoutGame3DGeometry.cpp \
           ../../../src/Train/WorkoutGame3DTerrainProfile.cpp \
           ../../../src/Train/WorkoutGameGapJumpGeometry.cpp \
           ../../../src/Train/WorkoutGameRoadCourse.cpp \
           ../../../src/Train/WorkoutGameRoadPlan.cpp \
           ../../../src/Train/WorkoutGameRoadQuality.cpp \
           ../../../src/Train/WorkoutGameFeatureChallenge.cpp

HEADERS += ../../../src/Train/WorkoutGame3DChunkBuilder.h \
           ../../../src/Train/WorkoutGame3DGeometry.h \
           ../../../src/Train/WorkoutGameClimbGeometry.h \
           ../../../src/Train/WorkoutGameTabletopGeometry.h \
           ../../../src/Train/WorkoutGame3DTerrainProfile.h \
           ../../../src/Train/WorkoutGameRoadCourse.h \
           ../../../src/Train/WorkoutGameFeatureCatalog.h \
           ../../../src/Train/WorkoutGameFeatureGeometry.h \
           ../../../src/Train/WorkoutGameRootGeometry.h \
           ../../../src/Train/WorkoutGameRockGardenGeometry.h \
           ../../../src/Train/WorkoutGameRockSlabGeometry.h \
           ../../../src/Train/WorkoutGameSkinnyGeometry.h \
           ../../../src/Train/WorkoutGameFeatureChallenge.h \
           ../../../src/Train/WorkoutGameTrailBranch.h \
           ../../../src/Train/WorkoutGameCourse.h \
           ../../../src/Train/WorkoutGameWorld.h

include(../../unittests.pri)

sanitize:!tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}

tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=thread -fno-omit-frame-pointer
    QMAKE_LFLAGS += -fsanitize=thread -no-pie
}
