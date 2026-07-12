QT += core gui widgets testlib xml sql network svg concurrent serialport \
      multimedia multimediawidgets webenginecore webenginewidgets webchannel \
      positioning webenginequick

greaterThan(QT_MAJOR_VERSION, 5): QT += core5compat

TEMPLATE = app
TARGET = tst_credentialSettings

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug
DEFINES += GC_CREDENTIAL_STORE_CUSTOM_FACTORY

unix:!android:!macx {
    PRECOMPILED_HEADER = ../../../src/stable.h
    CONFIG += precompile_header
}

SOURCES = testCredentialSettings.cpp \
          ../../../src/Core/CredentialSettings.cpp \
          ../../../src/Core/CredentialStoreQtKeychain.cpp \
          ../../../src/Core/Settings.cpp

HEADERS = ../../../src/Core/CredentialSettings.h \
          ../../../src/Core/CredentialStoreQtKeychain.h \
          ../../../src/Core/Settings.h

include(../../../contrib/qtkeychain/qtkeychain.pri)

INCLUDEPATH += ../../../src \
               ../../../contrib/qtkeychain \
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

QMAKE_CXXFLAGS += -ffunction-sections -fdata-sections
QMAKE_LFLAGS += -Wl,--gc-sections

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
