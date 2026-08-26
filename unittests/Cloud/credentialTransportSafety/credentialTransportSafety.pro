QT += core network testlib
QT -= gui

TEMPLATE = app
TARGET = tst_credentialTransportSafety

include(../../unittests.pri)

GC_TEST_SOURCE_ROOT = $$clean_path($$_PRO_FILE_PWD_/../../..)
DEFINES += GC_TEST_SOURCE_ROOT=\\\"$${GC_TEST_SOURCE_ROOT}\\\"

CONFIG += console testcase c++17 release
CONFIG -= debug app_bundle

SOURCES = testCredentialTransportSafety.cpp \
          ../../../src/Cloud/CloudCredentialTransport.cpp

HEADERS = ../../../src/Cloud/CloudCredentialTransport.h

INCLUDEPATH += ../../../src \
               ../../../src/Cloud

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
