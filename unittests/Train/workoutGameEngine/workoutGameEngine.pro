QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameEngine

SOURCES = testWorkoutGameEngine.cpp \
          ../../../src/Train/WorkoutGameAssetCatalog.cpp \
          ../../../src/Train/WorkoutGameAssetPhysicsResolver.cpp \
          ../../../src/Train/WorkoutGame3DTerrainProfile.cpp \
          ../../../src/Train/WorkoutGameAudioEvents.cpp \
          ../../../src/Train/WorkoutGameCompetition.cpp \
          ../../../src/Train/WorkoutGameCourse.cpp \
          ../../../src/Train/WorkoutGameCourseDocument.cpp \
          ../../../src/Train/WorkoutGameCourseConversion.cpp \
          ../../../src/Train/WorkoutGameCourseCrsExporter.cpp \
          ../../../src/Train/WorkoutGameCoursePrescription.cpp \
          ../../../src/Train/WorkoutGameCourseSummary.cpp \
          ../../../src/Train/WorkoutGameCourseTerrain.cpp \
          ../../../src/Train/WorkoutGameDistanceCourse.cpp \
          ../../../src/Train/WorkoutGameDistancePlayback.cpp \
          ../../../src/Train/WorkoutGameRoadPhysics.cpp \
          ../../../src/Train/WorkoutGameEngine.cpp \
          ../../../src/Train/WorkoutGameFeatureChallenge.cpp \
          ../../../src/Train/WorkoutGameFeatureLab.cpp \
          ../../../src/Train/WorkoutGameFeatureRuntime.cpp \
          ../../../src/Train/WorkoutGameGapJumpGeometry.cpp \
          ../../../src/Train/WorkoutGameGapJumpLaunchWindow.cpp \
          ../../../src/Train/WorkoutGameGapJumpSelector.cpp \
          ../../../src/Train/WorkoutGameRiderVisual.cpp \
          ../../../src/Train/WorkoutGameAssetPhysicsSampler.cpp \
          ../../../src/Train/WorkoutGameRoadCourse.cpp \
          ../../../src/Train/WorkoutGameAssetPhysicsSnapshot.cpp \
          ../../../src/Train/WorkoutGameRoadPlan.cpp \
          ../../../src/Train/WorkoutGameRoadQuality.cpp \
          ../../../src/Train/WorkoutGameSimulation.cpp \
          ../../../src/Train/TrainingDataGenerator.cpp \
          ../../../src/Train/WorkoutGameTerrainTransition.cpp \
          ../../../src/Train/WorkoutGameVisualSmoother.cpp \
          ../../../src/Train/WorkoutGameWorld.cpp \
          ../../../src/Train/WorkoutGameWorldGroundProfile.cpp

HEADERS = ../../../src/Train/WorkoutGame3DTerrainProfile.h \
          ../../../src/Train/WorkoutGameEngine.h \
          ../../../src/Train/WorkoutGameGapJumpGeometry.h \
          ../../../src/Train/WorkoutGameGapJumpLaunchWindow.h \
          ../../../src/Train/WorkoutGameGapJumpSelector.h \
          ../../../src/Train/TrainingDataGenerator.h \
          ../../../src/Train/WorkoutGameRiderVisual.h

RESOURCES += ../../../src/Resources/workout-game-assets.qrc

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
