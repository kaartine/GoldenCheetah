QT += testlib core

SOURCES = testPerspectiveStateSource.cpp \
          ../../../src/Gui/PerspectiveStateSource.cpp

RESOURCES = perspectiveStateSource.qrc

include(../../unittests.pri)
