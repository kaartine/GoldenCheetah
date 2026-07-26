QT += testlib core network
CONFIG += c++17

TARGET = testStravaAccountRemoval

SOURCES = testStravaAccountRemoval.cpp \
          ../../../src/Cloud/StravaAccountRemoval.cpp \
          ../../../src/Cloud/StravaRevocationClient.cpp \
          ../../../src/Cloud/StravaNetworkReply.cpp \
          ../../../src/Cloud/StravaOAuthPolicy.cpp \
          ../../../src/Cloud/NetworkReplyWait.cpp \
          ../../../src/Cloud/StravaTokenRefresh.cpp

HEADERS = ../../../src/Cloud/StravaAccountRemoval.h \
          ../../../src/Cloud/StravaCredentialPublisher.h \
          ../../../src/Cloud/StravaRevocationClient.h \
          ../../../src/Cloud/StravaNetworkReply.h \
          ../../../src/Cloud/StravaOAuthPolicy.h \
          ../../../src/Cloud/NetworkReplyWait.h \
          ../../../src/Cloud/StravaTokenPublication.h \
          ../../../src/Cloud/StravaTokenRefresh.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
