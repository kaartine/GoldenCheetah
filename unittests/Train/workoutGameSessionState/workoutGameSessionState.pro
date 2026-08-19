QT += testlib
QT -= gui
CONFIG += testcase c++17
TARGET = testWorkoutGameSessionState

SOURCES = testWorkoutGameSessionState.cpp \
          ../../../src/Train/WorkoutGameSessionState.cpp

HEADERS = ../../../src/Train/WorkoutGameSessionState.h

INCLUDEPATH += ../../../src
