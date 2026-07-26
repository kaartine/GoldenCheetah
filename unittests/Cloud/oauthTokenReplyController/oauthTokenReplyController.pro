QT += testlib core network
CONFIG += c++17

TARGET = testOAuthTokenReplyController

SOURCES = testOAuthTokenReplyController.cpp \
          ../../../src/Cloud/OAuthTokenReplyController.cpp

HEADERS = ../../../src/Cloud/OAuthTokenReplyController.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
