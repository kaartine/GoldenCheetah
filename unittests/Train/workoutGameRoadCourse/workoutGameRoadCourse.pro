QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameRoadCourse

SOURCES = testWorkoutGameRoadCourse.cpp \
          ../../../src/Train/WorkoutGameFeatureChallenge.cpp \
          ../../../src/Train/WorkoutGameRoadCourse.cpp \
          ../../../src/Train/WorkoutGameRoadProjection.cpp

HEADERS = ../../../src/Train/WorkoutGameCourse.h \
          ../../../src/Train/WorkoutGameFeatureCatalog.h \
          ../../../src/Train/WorkoutGameFeatureChallenge.h \
          ../../../src/Train/WorkoutGameRoadCourse.h \
          ../../../src/Train/WorkoutGameRoadProjection.h \
          ../../../src/Train/WorkoutGameWorld.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
