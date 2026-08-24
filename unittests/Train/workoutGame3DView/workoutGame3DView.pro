QT += core gui quick quick3d testlib
CONFIG += testcase console c++17
TEMPLATE = app
TARGET = testWorkoutGame3DView

INCLUDEPATH += ../../../src/Train

SOURCES += testWorkoutGame3DView.cpp \
           ../../../src/Train/WorkoutGame3DGeometry.cpp \
           ../../../src/Train/WorkoutGame3DViewModel.cpp \
           ../../../src/Train/WorkoutGame3DWindow.cpp \
           ../../../src/Train/WorkoutGameRoadCourse.cpp \
           ../../../src/Train/WorkoutGameFeatureChallenge.cpp \
           ../../../src/Train/WorkoutGameFeatureRuntime.cpp \
           ../../../src/Train/WorkoutGameTerrainTransition.cpp \
           ../../../src/Train/WorkoutGameVisualSmoother.cpp

HEADERS += ../../../src/Train/WorkoutGame3DGeometry.h \
           ../../../src/Train/WorkoutGame3DViewModel.h \
           ../../../src/Train/WorkoutGame3DWindow.h \
           ../../../src/Train/WorkoutGameRoadCourse.h \
           ../../../src/Train/WorkoutGameFeatureCatalog.h \
           ../../../src/Train/WorkoutGameFeatureGeometry.h \
           ../../../src/Train/WorkoutGameFeatureChallenge.h \
           ../../../src/Train/WorkoutGameCourse.h \
           ../../../src/Train/WorkoutGameEngine.h \
           ../../../src/Train/WorkoutGameFeatureRuntime.h \
           ../../../src/Train/WorkoutGameSimulation.h \
           ../../../src/Train/WorkoutGameVisualSmoother.h \
           ../../../src/Train/WorkoutGameWorld.h

RESOURCES += workoutGame3DView.qrc

include(../../unittests.pri)
