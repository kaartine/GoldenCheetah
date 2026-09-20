QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameAssetPhysicsSampler

SOURCES = testWorkoutGameAssetPhysicsSampler.cpp \
          ../../../src/Train/WorkoutGameAssetPhysicsSampler.cpp \
          ../../../src/Train/WorkoutGameAssetPhysicsSnapshot.cpp

HEADERS = ../../../src/Train/WorkoutGameAssetPhysicsSampler.h \
          ../../../src/Train/WorkoutGameAssetPhysicsSnapshot.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
