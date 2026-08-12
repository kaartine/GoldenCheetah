QT += core testlib

TEMPLATE = app
TARGET = tst_guiSmokeShutdown

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testGuiSmokeShutdown.cpp \
          ../../../src/Gui/GuiSmokeShutdown.cpp

HEADERS = ../../../src/Gui/GuiSmokeShutdown.h

INCLUDEPATH += ../../../src/Gui

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
