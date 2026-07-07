QT += testlib core
CONFIG += c++17

TARGET = testBluetoothTelemetryRouter

SOURCES = testBluetoothTelemetryRouter.cpp \
          ../../../src/Train/BluetoothTelemetryRouter.cpp

HEADERS = ../../../src/Train/BluetoothTelemetryRouter.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
