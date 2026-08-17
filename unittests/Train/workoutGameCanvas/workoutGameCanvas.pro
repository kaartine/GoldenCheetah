QT += core gui widgets testlib opengl openglwidgets
CONFIG += c++17

TARGET = testWorkoutGameCanvas

SOURCES = testWorkoutGameCanvas.cpp \
          ../../../src/Train/WorkoutGameCanvas.cpp \
          ../../../src/Train/WorkoutGameCompetition.cpp \
          ../../../src/Train/WorkoutGameCourse.cpp \
          ../../../src/Train/WorkoutGameOpenGLCanvas.cpp \
          ../../../src/Train/WorkoutGameSimulation.cpp

HEADERS = ../../../src/Train/WorkoutGameCanvas.h \
          ../../../src/Train/WorkoutGameCompetition.h \
          ../../../src/Train/WorkoutGameCourse.h \
          ../../../src/Train/WorkoutGameOpenGLCanvas.h \
          ../../../src/Train/WorkoutGameSimulation.h

RESOURCES = workoutGameCanvas.qrc

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
