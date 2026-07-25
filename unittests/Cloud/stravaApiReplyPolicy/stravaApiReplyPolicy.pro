QT += testlib core network
CONFIG += c++17

TARGET = testStravaApiReplyPolicy

SOURCES = testStravaApiReplyPolicy.cpp \
          ../../../src/Cloud/StravaApiReplyPolicy.cpp

HEADERS = ../../../src/Cloud/StravaApiReplyPolicy.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
