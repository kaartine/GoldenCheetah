QT += core testlib

TEMPLATE = app
TARGET = tst_localApiFileStore

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES += testLocalApiFileStore.cpp
SOURCES += ../../../src/Core/LocalApiFileStore.cpp
SOURCES += ../../../src/Core/LocalApiEndpointInput.cpp
SOURCES += ../../../src/FileIO/AnchoredFileSystem.cpp

HEADERS += ../../../src/Core/LocalApiFileStore.h
HEADERS += ../../../src/Core/LocalApiEndpointInput.h
HEADERS += ../../../src/FileIO/AnchoredFileSystem.h

INCLUDEPATH += ../../../src/FileIO

win32:LIBS += -ladvapi32

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
