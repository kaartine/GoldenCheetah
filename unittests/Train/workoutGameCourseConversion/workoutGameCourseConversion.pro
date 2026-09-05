QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameCourseConversion

SOURCES = testWorkoutGameCourseConversion.cpp \
          ../../../src/Train/WorkoutGameCourse.cpp \
          ../../../src/Train/WorkoutGameCourseConversion.cpp \
          ../../../src/Train/WorkoutGameCoursePrescription.cpp \
          ../../../src/Train/WorkoutGameCourseSummary.cpp \
          ../../../src/Train/WorkoutGameCourseTerrain.cpp \
          ../../../src/Train/WorkoutGameDistanceCourse.cpp \
          ../../../src/Train/WorkoutGameRoadPhysics.cpp

HEADERS = ../../../src/Train/WorkoutGameCourse.h \
          ../../../src/Train/WorkoutGameCourseConversion.h \
          ../../../src/Train/WorkoutGameCoursePrescription.h \
          ../../../src/Train/WorkoutGameCourseSummary.h \
          ../../../src/Train/WorkoutGameCourseTerrain.h \
          ../../../src/Train/WorkoutGameDistanceCourse.h \
          ../../../src/Train/WorkoutGameRoadPhysics.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
