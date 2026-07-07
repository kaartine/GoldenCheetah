QT += testlib core bluetooth
CONFIG += c++17

TARGET = testFtmsTargetReadiness

SOURCES = testFtmsTargetReadiness.cpp \
          ../../../src/Train/Ftms.cpp

HEADERS = ../../../src/Train/Ftms.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
