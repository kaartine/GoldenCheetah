QT += core gui quick quick3d testlib
CONFIG += testcase console c++17
TEMPLATE = app
TARGET = testWorkoutGame3DView

INCLUDEPATH += ../../../src/Train

SOURCES += testWorkoutGame3DView.cpp \
           ../../../src/Train/WorkoutGame3DFeatureAsset.cpp \
           ../../../src/Train/WorkoutGame3DGeometry.cpp \
           ../../../src/Train/WorkoutGame3DTerrainProfile.cpp \
           ../../../src/Train/WorkoutGame3DViewModel.cpp \
           ../../../src/Train/WorkoutGame3DWindow.cpp \
           ../../../src/Train/WorkoutGameRoadCourse.cpp \
           ../../../src/Train/WorkoutGameFeatureChallenge.cpp \
           ../../../src/Train/WorkoutGameFeatureHud.cpp \
           ../../../src/Train/WorkoutGameFeatureRuntime.cpp \
           ../../../src/Train/WorkoutGameSimulation.cpp \
           ../../../src/Train/WorkoutGameTerrainTransition.cpp \
           ../../../src/Train/WorkoutGameVisualSmoother.cpp \
           ../../../src/Train/WorkoutGameWorld.cpp

HEADERS += ../../../src/Train/WorkoutGame3DGeometry.h \
           ../../../src/Train/WorkoutGameClimbGeometry.h \
           ../../../src/Train/WorkoutGameTabletopGeometry.h \
           ../../../src/Train/WorkoutGame3DFeatureAsset.h \
           ../../../src/Train/WorkoutGame3DTerrainProfile.h \
           ../../../src/Train/WorkoutGame3DViewModel.h \
           ../../../src/Train/WorkoutGame3DWindow.h \
           ../../../src/Train/WorkoutGameRoadCourse.h \
           ../../../src/Train/WorkoutGameFeatureCatalog.h \
           ../../../src/Train/WorkoutGameFeatureGeometry.h \
           ../../../src/Train/WorkoutGameRootGeometry.h \
           ../../../src/Train/WorkoutGameRockGardenGeometry.h \
           ../../../src/Train/WorkoutGameRockSlabGeometry.h \
           ../../../src/Train/WorkoutGameSkinnyGeometry.h \
           ../../../src/Train/WorkoutGameFeatureChallenge.h \
           ../../../src/Train/WorkoutGameCourse.h \
           ../../../src/Train/WorkoutGameEngine.h \
           ../../../src/Train/WorkoutGameFeatureHud.h \
           ../../../src/Train/WorkoutGameFeatureRuntime.h \
           ../../../src/Train/WorkoutGameSimulation.h \
           ../../../src/Train/WorkoutGameVisualSmoother.h \
           ../../../src/Train/WorkoutGameWorld.h

BOX2D_ROOT = $$clean_path($$_PRO_FILE_PWD_/../../../vendor/box2d-3.1.1)
include($$BOX2D_ROOT/box2d.pri)

RESOURCES += workoutGame3DView.qrc \
             ../../../src/Resources/workout-game-assets.qrc

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
