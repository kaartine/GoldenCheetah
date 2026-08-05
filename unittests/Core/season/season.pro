QT += testlib widgets core5compat

SOURCES = testSeason.cpp \
          ../../../src/Core/Season.cpp \
          ../../../src/Core/Utils.cpp

HEADERS = ../../../src/Core/Season.h \
          ../../../src/Core/Utils.h

INCLUDEPATH += ../../../src \
               ../../../src/Core

include(../../unittests.pri)
