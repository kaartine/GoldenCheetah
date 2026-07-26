QT += testlib core
CONFIG += c++17

TARGET = testStravaTokenPublication

SOURCES = testStravaTokenPublication.cpp \
          ../../../src/Cloud/StravaTokenPublication.cpp

HEADERS = ../../../src/Cloud/StravaTokenPublication.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
