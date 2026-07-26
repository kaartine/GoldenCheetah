QT += testlib core network
CONFIG += c++17

TARGET = testStravaRoutesClient

SOURCES = testStravaRoutesClient.cpp \
          ../../../src/Train/StravaRoutesClient.cpp

HEADERS = ../../../src/Train/StravaRoutesClient.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
