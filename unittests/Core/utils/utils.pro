QT += testlib core widgets core5compat

SOURCES = testUtils.cpp \
          ../../../src/Core/Utils.cpp

HEADERS = ../../../src/Core/Utils.h

INCLUDEPATH += ../../../src \
               ../../../src/Core

include(../../unittests.pri)
