QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameAssetPhysicsResolver

SOURCES = testWorkoutGameAssetPhysicsResolver.cpp \
          ../../../src/Train/WorkoutGameAssetCatalog.cpp \
          ../../../src/Train/WorkoutGameAssetPhysicsResolver.cpp \
          ../../../src/Train/WorkoutGameAssetPhysicsSampler.cpp \
          ../../../src/Train/WorkoutGameAssetPhysicsSnapshot.cpp \
          ../../../src/Train/WorkoutGameRoadPlan.cpp \
          ../../../src/Train/WorkoutGameRoadQuality.cpp

HEADERS = ../../../src/Train/WorkoutGameAssetCatalog.h \
          ../../../src/Train/WorkoutGameAssetPhysicsResolver.h \
          ../../../src/Train/WorkoutGameAssetPhysicsSampler.h \
          ../../../src/Train/WorkoutGameAssetPhysicsSnapshot.h \
          ../../../src/Train/WorkoutGameFt02PhysicsV1.h \
          ../../../src/Train/WorkoutGameRoadPlan.h \
          ../../../src/Train/WorkoutGameRoadQuality.h

RESOURCES += ../../../src/Resources/workout-game-assets.qrc

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
