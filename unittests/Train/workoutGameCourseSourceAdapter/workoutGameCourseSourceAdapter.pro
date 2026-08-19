QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameCourseSourceAdapter

SOURCES = testWorkoutGameCourseSourceAdapter.cpp \
          ../../../src/Train/WorkoutGameCourse.cpp \
          ../../../src/Train/WorkoutGameCourseConversion.cpp \
          ../../../src/Train/WorkoutGameCourseCrsExporter.cpp \
          ../../../src/Train/WorkoutGameCourseDocument.cpp \
          ../../../src/Train/WorkoutGameCourseSourceAdapter.cpp \
          ../../../src/Train/WorkoutGameDistanceCourse.cpp \
          ../../../src/Train/WorkoutGameRoadPhysics.cpp \
          ../../../src/Train/WorkoutGameWorkoutAdapter.cpp

HEADERS = ../../../src/Train/WorkoutGameCourse.h \
          ../../../src/Train/WorkoutGameCourseConversion.h \
          ../../../src/Train/WorkoutGameCourseCrsExporter.h \
          ../../../src/Train/WorkoutGameCourseDocument.h \
          ../../../src/Train/WorkoutGameCourseSourceAdapter.h \
          ../../../src/Train/WorkoutGameDistanceCourse.h \
          ../../../src/Train/WorkoutGameRoadPhysics.h \
          ../../../src/Train/WorkoutGameWorkoutAdapter.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
