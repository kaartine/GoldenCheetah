QT += core testlib

TEMPLATE = app
TARGET = tst_guiStartupPolicy

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testGuiStartupPolicy.cpp \
          ../../../src/Gui/GuiStartupPolicy.cpp

HEADERS = ../../../src/Gui/GuiStartupPolicy.h

INCLUDEPATH += ../../../src/Gui

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
