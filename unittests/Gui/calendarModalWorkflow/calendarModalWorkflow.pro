TEMPLATE = app
TARGET = tst_calendarModalWorkflow

CONFIG += console testcase c++17 release
CONFIG -= debug

QT += core gui widgets testlib

SOURCES = testCalendarModalWorkflow.cpp \
          ../../../src/Core/Season.cpp

INCLUDEPATH += ../../../src \
               ../../../src/Charts \
               ../../../src/Core \
               ../../../src/Gui

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}

include(../../unittests.pri)
