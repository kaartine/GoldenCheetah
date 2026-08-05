QT += core gui widgets testlib xml sql network svg concurrent serialport \
      multimedia multimediawidgets webenginecore webenginewidgets webchannel \
      positioning webenginequick

greaterThan(QT_MAJOR_VERSION, 5): QT += core5compat

TEMPLATE = app
TARGET = tst_credentialSettings

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug
DEFINES += GC_CREDENTIAL_STORE_CUSTOM_FACTORY \
           GC_SETTINGS_NO_GLOBAL_INSTANCE \
           GC_CREDENTIAL_TEST_HOOKS

unix:!android:!macx {
    PRECOMPILED_HEADER = ../../../src/stable.h
    CONFIG += precompile_header
}

SOURCES = testCredentialSettings.cpp \
          ../../../src/Core/CredentialSettings.cpp \
          ../../../src/Core/CredentialStoreQtKeychain.cpp \
          ../../../src/Core/Settings.cpp \
          ../../../src/FileIO/AnchoredFileSystem.cpp

HEADERS = ../../../src/Core/CredentialSettings.h \
          ../../../src/Core/CredentialStoreQtKeychain.h \
          ../../../src/Core/Settings.h \
          ../../../src/FileIO/AnchoredFileSystem.h

include(../../../contrib/qtkeychain/qtkeychain.pri)

win32:LIBS += -ladvapi32

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

sanitize:!tsan:!msvc {
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
    QMAKE_LFLAGS += -fsanitize=thread \
                    -no-pie
}
