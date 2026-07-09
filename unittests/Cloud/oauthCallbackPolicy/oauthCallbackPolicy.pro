QT += testlib core network
CONFIG += c++17

TARGET = testOAuthCallbackPolicy

SOURCES = testOAuthCallbackPolicy.cpp \
          ../../../src/Cloud/OAuthCallbackPolicy.cpp

HEADERS = ../../../src/Cloud/OAuthCallbackPolicy.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
