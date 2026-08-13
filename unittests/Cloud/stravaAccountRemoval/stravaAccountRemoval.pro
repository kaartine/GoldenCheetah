QT += testlib core gui widgets network
CONFIG += c++17

TARGET = testStravaAccountRemoval

SOURCES = testStravaAccountRemoval.cpp \
          StravaPublisherTestSettings.cpp \
          ../../../src/Cloud/StravaAccountRemoval.cpp \
          ../../../src/Cloud/StravaCredentialPublisher.cpp \
          ../../../src/Cloud/StravaSettingsCommit.cpp \
          ../../../src/Cloud/StravaRevocationClient.cpp \
          ../../../src/Cloud/StravaNetworkReply.cpp \
          ../../../src/Cloud/StravaOAuthPolicy.cpp \
          ../../../src/Cloud/NetworkReplyWait.cpp \
          ../../../src/Cloud/StravaTokenRefresh.cpp \
          ../../../src/Cloud/StravaTokenPublication.cpp \
          ../../../src/Cloud/StravaCredentialDurability.cpp \
          ../../../src/FileIO/AnchoredFileSystem.cpp

HEADERS = ../../../src/Cloud/StravaAccountRemoval.h \
          StravaPublisherTestSettings.h \
          ../../../src/Cloud/StravaCredentialPublisher.h \
          ../../../src/Cloud/StravaSettingsCommit.h \
          ../../../src/Cloud/StravaRevocationClient.h \
          ../../../src/Cloud/StravaNetworkReply.h \
          ../../../src/Cloud/StravaOAuthPolicy.h \
          ../../../src/Cloud/NetworkReplyWait.h \
          ../../../src/Cloud/StravaTokenPublication.h \
          ../../../src/Cloud/StravaCredentialDurability.h \
          ../../../src/Cloud/StravaTokenRefresh.h

INCLUDEPATH += ../../../src \
               ../../../src/ANT \
               ../../../src/Charts \
               ../../../src/Cloud \
               ../../../src/Core \
               ../../../src/FileIO \
               ../../../src/Gui \
               ../../../src/Metrics \
               ../../../src/Planning \
               ../../../src/Train \
               ../../../qwt/src

win32:LIBS += -ladvapi32

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
