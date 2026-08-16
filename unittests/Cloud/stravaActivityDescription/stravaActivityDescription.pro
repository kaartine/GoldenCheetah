QT += testlib core
CONFIG += c++17

TARGET = testStravaActivityDescription

SOURCES = testStravaActivityDescription.cpp \
          ../../../src/Cloud/StravaActivityDescription.cpp

HEADERS = ../../../src/Cloud/StravaActivityDescription.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
