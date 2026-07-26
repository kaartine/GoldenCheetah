QT += testlib core network
CONFIG += c++17

TARGET = testStravaAuthenticatedSession

SOURCES = testStravaAuthenticatedSession.cpp \
          ../../../src/Cloud/StravaAuthenticatedSession.cpp \
          ../../../src/Cloud/StravaNetworkReply.cpp \
          ../../../src/Cloud/NetworkReplyWait.cpp

HEADERS = ../../../src/Cloud/StravaAuthenticatedSession.h \
          ../../../src/Cloud/StravaNetworkReply.h \
          ../../../src/Cloud/NetworkReplyWait.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
