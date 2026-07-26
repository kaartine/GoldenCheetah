QT += testlib core bluetooth
CONFIG += c++17

TARGET = testKineticPacketBounds

contains(QMAKE_COMPILER, gcc):!contains(QMAKE_COMPILER, clang) {
    QMAKE_CXXFLAGS += -O2 -fstrict-aliasing \
                      -Wstrict-aliasing=2 -Werror=strict-aliasing
}

SOURCES = testKineticPacketBounds.cpp \
          ../../../src/Train/KurtInRide.cpp \
          ../../../src/Train/KurtSmartControl.cpp

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
