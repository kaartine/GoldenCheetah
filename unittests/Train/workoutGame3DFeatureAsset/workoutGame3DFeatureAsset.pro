QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGame3DFeatureAsset

SOURCES = testWorkoutGame3DFeatureAsset.cpp \
          ../../../src/Train/WorkoutGame3DFeatureAsset.cpp \
          ../../../src/Train/WorkoutGameRoadCourse.cpp \
          ../../../src/Train/WorkoutGameFeatureChallenge.cpp

HEADERS = ../../../src/Train/WorkoutGame3DFeatureAsset.h \
          ../../../src/Train/WorkoutGameRoadCourse.h \
          ../../../src/Train/WorkoutGameFeatureGeometry.h

include(../../unittests.pri)
