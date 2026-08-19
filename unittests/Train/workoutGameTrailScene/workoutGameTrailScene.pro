QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameTrailScene

SOURCES = testWorkoutGameTrailScene.cpp \
          ../../../src/Train/WorkoutGameTrailScene.cpp \
          ../../../src/Train/WorkoutGameWorld.cpp

HEADERS = ../../../src/Train/WorkoutGameTrailScene.h \
          ../../../src/Train/WorkoutGameWorld.h

BOX2D_ROOT = $$clean_path($$_PRO_FILE_PWD_/../../../vendor/box2d-3.1.1)
include($$BOX2D_ROOT/box2d.pri)

include(../../unittests.pri)
