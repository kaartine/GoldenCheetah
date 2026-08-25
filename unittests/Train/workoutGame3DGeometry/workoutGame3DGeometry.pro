QT += core gui testlib quick3d
CONFIG += testcase console c++17
TEMPLATE = app
TARGET = testWorkoutGame3DGeometry

INCLUDEPATH += ../../../src/Train

SOURCES += testWorkoutGame3DGeometry.cpp \
           ../../../src/Train/WorkoutGame3DGeometry.cpp \
           ../../../src/Train/WorkoutGame3DTerrainProfile.cpp \
           ../../../src/Train/WorkoutGameRoadCourse.cpp \
           ../../../src/Train/WorkoutGameFeatureChallenge.cpp

HEADERS += ../../../src/Train/WorkoutGame3DGeometry.h \
           ../../../src/Train/WorkoutGame3DTerrainProfile.h \
           ../../../src/Train/WorkoutGameRoadCourse.h \
           ../../../src/Train/WorkoutGameFeatureCatalog.h \
           ../../../src/Train/WorkoutGameFeatureGeometry.h \
           ../../../src/Train/WorkoutGameFeatureChallenge.h \
           ../../../src/Train/WorkoutGameCourse.h \
           ../../../src/Train/WorkoutGameWorld.h

include(../../unittests.pri)
