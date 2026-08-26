QT += core network testlib

TEMPLATE = app
TARGET = tst_stravaClientCredentials

include(../../unittests.pri)

GC_TEST_SOURCE_ROOT = $$clean_path($$_PRO_FILE_PWD_/../../..)
DEFINES += GC_TEST_SOURCE_ROOT=\\\"$${GC_TEST_SOURCE_ROOT}\\\"

CONFIG += console testcase c++17 release
CONFIG -= debug
DEFINES += STRAVA_DEBUG=1

SOURCES = testStravaClientCredentials.cpp \
          ../../../src/Cloud/StravaClientCredentials.cpp \
          ../../../src/Cloud/StravaOAuthPolicy.cpp \
          ../../../src/Cloud/StravaRevocationClient.cpp

HEADERS = ../../../src/Cloud/StravaClientCredentials.h \
          ../../../src/Cloud/StravaOAuthPolicy.h \
          ../../../src/Cloud/StravaRevocationClient.h \
          ../../../src/Cloud/StravaNetworkReply.h

INCLUDEPATH += ../../../src \
               ../../../src/Cloud

sanitize:!tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
