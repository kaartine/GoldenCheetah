QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameCourseDocument

SOURCES = testWorkoutGameCourseDocument.cpp \
          ../../../src/Train/WorkoutGameCourse.cpp \
          ../../../src/Train/WorkoutGameCourseConversion.cpp \
          ../../../src/Train/WorkoutGameCourseCrsExporter.cpp \
          ../../../src/Train/WorkoutGameCourseDocument.cpp \
          ../../../src/Train/WorkoutGameDistanceCourse.cpp \
          ../../../src/Train/WorkoutGameDistancePlayback.cpp \
          ../../../src/Train/WorkoutGameFeatureChallenge.cpp \
          ../../../src/Train/WorkoutGameGapJumpGeometry.cpp \
          ../../../src/Train/WorkoutGameRoadCourse.cpp \
          ../../../src/Train/WorkoutGameRoadPlan.cpp \
          ../../../src/Train/WorkoutGameRoadQuality.cpp \
          ../../../src/Train/WorkoutGameRoadPhysics.cpp

HEADERS = ../../../src/Train/WorkoutGameCourse.h \
          ../../../src/Train/WorkoutGameCourseConversion.h \
          ../../../src/Train/WorkoutGameCourseCrsExporter.h \
          ../../../src/Train/WorkoutGameCourseDocument.h \
          ../../../src/Train/WorkoutGameDistanceCourse.h \
          ../../../src/Train/WorkoutGameDistancePlayback.h \
          ../../../src/Train/WorkoutGameRoadCourse.h \
          ../../../src/Train/WorkoutGameRoadPlan.h \
          ../../../src/Train/WorkoutGameRoadQuality.h \
          ../../../src/Train/WorkoutGameRoadPhysics.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
