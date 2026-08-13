QT += core gui testlib

TEMPLATE = app
TARGET = tst_openGLVersionProbe

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testOpenGLVersionProbe.cpp \
          ../../../src/Gui/OpenGLVersionProbe.cpp

HEADERS = ../../../src/Gui/OpenGLVersionProbe.h

INCLUDEPATH += ../../../src/Gui

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
