QT += testlib core

SOURCES = testUnits.cpp \
          ../../../src/Core/Units.cpp

HEADERS = ../../../src/Core/Units.h

INCLUDEPATH += ../../../src \
               ../../../src/Core

include(../../unittests.pri)
