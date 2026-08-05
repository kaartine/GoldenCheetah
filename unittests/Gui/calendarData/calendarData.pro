QT += testlib core gui

SOURCES = testCalendarData.cpp \
          ../../../src/Gui/CalendarData.cpp

HEADERS = ../../../src/Gui/CalendarData.h

INCLUDEPATH += ../../../src \
               ../../../src/Gui

include(../../unittests.pri)
