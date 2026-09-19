QT += core concurrent testlib
QT -= gui
CONFIG += testcase c++17
TARGET = testWorkoutGameDevelopmentAssets

SOURCES = testWorkoutGameDevelopmentAssets.cpp \
          ../../../src/Train/WorkoutGameDevelopmentAssets.cpp

HEADERS = ../../../src/Train/WorkoutGameDevelopmentAssets.h

INCLUDEPATH += ../../../src/Train
