QT += core testlib
CONFIG += c++17

TARGET = testWorkoutGameAssetCatalog

SOURCES = testWorkoutGameAssetCatalog.cpp \
          ../../../src/Train/WorkoutGameAssetCatalog.cpp

HEADERS = ../../../src/Train/WorkoutGameAssetCatalog.h

RESOURCES += ../../../src/Resources/workout-game-assets.qrc

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
