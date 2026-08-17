QT += core testlib
CONFIG += c++17

TARGET = testTrainerTargetCoordinator

SOURCES = testTrainerTargetCoordinator.cpp \
          ../../../src/Train/TrainerTargetCoordinator.cpp

HEADERS = ../../../src/Train/TrainerTargetCoordinator.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
