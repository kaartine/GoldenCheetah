QT += core gui widgets testlib xml sql network svg concurrent serialport \
      multimedia multimediawidgets webenginecore webenginewidgets webchannel \
      positioning webenginequick
QT += charts
greaterThan(QT_MAJOR_VERSION, 5): QT += core5compat
CONFIG += c++17
DEFINES += GC_OAUTH_DIALOG_TEST_HOOKS

TARGET = testStravaOAuthPolicy

include(../../unittests.pri)

GC_TEST_SOURCE_ROOT = $$clean_path($$_PRO_FILE_PWD_/../../..)
DEFINES += GC_TEST_SOURCE_ROOT=\\\"$${GC_TEST_SOURCE_ROOT}\\\"

SOURCES = testStravaOAuthPolicy.cpp \
          OAuthDialogTestStubs.cpp \
          ../stravaAccountRemoval/StravaPublisherTestSettings.cpp \
          ../../../src/Cloud/OAuthDialog.cpp \
          ../../../src/Cloud/OAuthDialogMessageGuard.cpp \
          ../../../src/Cloud/OAuthCallbackPolicy.cpp \
          ../../../src/Cloud/OAuthPKCE.cpp \
          ../../../src/Cloud/OAuthTokenReplyController.cpp \
          ../../../src/Cloud/CloudCredentialTransport.cpp \
          ../../../src/Cloud/NetworkReplyWait.cpp \
          ../../../src/Cloud/StravaCredentialDurability.cpp \
          ../../../src/Cloud/StravaClientCredentials.cpp \
          ../../../src/Cloud/StravaClientCredentialsSettings.cpp \
          ../../../src/Cloud/StravaCredentialPublisher.cpp \
          ../../../src/Cloud/StravaOAuthPolicy.cpp \
          ../../../src/Cloud/StravaSettingsCommit.cpp \
          ../../../src/Cloud/StravaTokenPublication.cpp \
          ../../../src/Cloud/StravaTokenRefresh.cpp \
          ../../../src/FileIO/AnchoredFileSystem.cpp

HEADERS = ../stravaAccountRemoval/StravaPublisherTestSettings.h \
          ../../../src/Cloud/OAuthDialog.h \
          ../../../src/Cloud/OAuthDialogMessageGuard.h \
          ../../../src/Cloud/OAuthCallbackPolicy.h \
          ../../../src/Cloud/OAuthPKCE.h \
          ../../../src/Cloud/OAuthTokenReplyController.h \
          ../../../src/Cloud/NetworkReplyWait.h \
          ../../../src/Cloud/StravaCredentialDurability.h \
          ../../../src/Cloud/StravaClientCredentials.h \
          ../../../src/Cloud/StravaCredentialPublisher.h \
          ../../../src/Cloud/StravaOAuthPolicy.h \
          ../../../src/Cloud/StravaSettingsCommit.h \
          ../../../src/Cloud/StravaTokenPublication.h \
          ../../../src/Cloud/StravaTokenRefresh.h \
          ../../../src/FileIO/AnchoredFileSystem.h

INCLUDEPATH += ../../../src \
               $${GSL_INCLUDES} \
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

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
