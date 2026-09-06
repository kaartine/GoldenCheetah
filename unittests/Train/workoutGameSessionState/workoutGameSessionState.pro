QT += testlib
QT -= gui
CONFIG += testcase c++17
TARGET = testWorkoutGameSessionState

SOURCES = testWorkoutGameSessionState.cpp \
          ../../../src/Train/WorkoutGameSessionState.cpp \
          ../../../src/Train/WorkoutGameWorkoutIdentity.cpp

HEADERS = ../../../src/Train/WorkoutGameSessionState.h \
          ../../../src/Train/WorkoutGameWorkoutIdentity.h

INCLUDEPATH += ../../../src
