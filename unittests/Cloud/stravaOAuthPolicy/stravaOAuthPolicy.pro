QT += testlib core network
CONFIG += c++17

TARGET = testStravaOAuthPolicy

SOURCES = testStravaOAuthPolicy.cpp \
          ../../../src/Cloud/StravaOAuthPolicy.cpp

HEADERS = ../../../src/Cloud/StravaOAuthPolicy.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
