QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGamePowerProfile

SOURCES = testWorkoutGamePowerProfile.cpp \
          ../../../src/Train/WorkoutGameFeatureChallenge.cpp \
          ../../../src/Train/WorkoutGameFeatureLab.cpp \
          ../../../src/Train/WorkoutGameGapJumpGeometry.cpp \
          ../../../src/Train/WorkoutGamePowerProfile.cpp

HEADERS = ../../../src/Train/WorkoutGameFeatureChallenge.h \
          ../../../src/Train/WorkoutGameFeatureLab.h \
          ../../../src/Train/WorkoutGameGapJumpGeometry.h \
          ../../../src/Train/WorkoutGamePowerProfile.h

include(../../unittests.pri)
