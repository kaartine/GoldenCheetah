QT += core testlib
CONFIG += testcase console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = tst_stravaCredentialDurability

include(../../unittests.pri)

INCLUDEPATH += ../../../src \
               ../../../src/Core \
               ../../../src/FileIO

DEFINES += GC_STRAVA_CREDENTIAL_TEST_HOOKS

SOURCES = testStravaCredentialDurability.cpp \
          ../../../src/Cloud/StravaCredentialDurability.cpp \
          ../../../src/Cloud/StravaSettingsCommit.cpp \
          ../../../src/Cloud/StravaTokenPublication.cpp \
          ../../../src/FileIO/AnchoredFileSystem.cpp

HEADERS = ../../../src/Cloud/StravaCredentialDurability.h \
          ../../../src/Cloud/StravaSettingsCommit.h \
          ../../../src/Cloud/StravaTokenPublication.h \
          ../../../src/FileIO/AnchoredFileSystem.h \
          ../../../src/FileIO/AtomicFileWriter.h

win32:LIBS += -ladvapi32

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}

tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=thread \
                      -fno-omit-frame-pointer \
                      -fno-pie \
                      -O1 \
                      -g
    QMAKE_LFLAGS += -fsanitize=thread -no-pie
}
