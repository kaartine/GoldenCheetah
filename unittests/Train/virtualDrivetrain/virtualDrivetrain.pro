QT += core testlib
CONFIG += c++17

TARGET = testVirtualDrivetrain

SOURCES = testVirtualDrivetrain.cpp \
          ../../../src/Train/VirtualDrivetrain.cpp

HEADERS = ../../../src/Train/VirtualDrivetrain.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
