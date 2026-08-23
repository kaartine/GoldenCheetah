QT += core gui widgets testlib opengl openglwidgets
CONFIG += c++17

TARGET = testWorkoutGameCanvas

SOURCES = testWorkoutGameCanvas.cpp \
          ../../../src/Train/WorkoutGameCanvas.cpp \
          ../../../src/Train/WorkoutGameClock.cpp \
          ../../../src/Train/WorkoutGameCompetition.cpp \
          ../../../src/Train/WorkoutGameCourse.cpp \
          ../../../src/Train/WorkoutGameFeatureChallenge.cpp \
          ../../../src/Train/WorkoutGameOpenGLCanvas.cpp \
          ../../../src/Train/WorkoutGamePowerProfile.cpp \
          ../../../src/Train/WorkoutGameRoadCourse.cpp \
          ../../../src/Train/WorkoutGameSimulation.cpp \
          ../../../src/Train/WorkoutGameTerrainTransition.cpp \
          ../../../src/Train/WorkoutGameTrailScene.cpp \
          ../../../src/Train/WorkoutGameVisualSmoother.cpp \
          ../../../src/Train/WorkoutGameWorld.cpp

HEADERS = ../../../src/Train/WorkoutGameCanvas.h \
          ../../../src/Train/WorkoutGameClock.h \
          ../../../src/Train/WorkoutGameCompetition.h \
          ../../../src/Train/WorkoutGameCourse.h \
          ../../../src/Train/WorkoutGameFeatureChallenge.h \
          ../../../src/Train/WorkoutGameOpenGLCanvas.h \
          ../../../src/Train/WorkoutGamePowerProfile.h \
          ../../../src/Train/WorkoutGameRoadCourse.h \
          ../../../src/Train/WorkoutGameSimulation.h \
          ../../../src/Train/WorkoutGameTerrainTransition.h \
          ../../../src/Train/WorkoutGameTrailScene.h \
          ../../../src/Train/WorkoutGameVisualSmoother.h \
          ../../../src/Train/WorkoutGameWorld.h

RESOURCES = workoutGameCanvas.qrc

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
