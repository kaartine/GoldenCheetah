QT += testlib widgets

SOURCES = testSeasonOffset.cpp \
          ../../../src/Core/Season.cpp

HEADERS = ../../../src/Core/Season.h

INCLUDEPATH += ../../../src \
               ../../../src/Core

include(../../unittests.pri)
