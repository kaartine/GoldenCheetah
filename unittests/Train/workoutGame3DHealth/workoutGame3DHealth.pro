QT += core testlib
CONFIG += testcase console c++17
TEMPLATE = app
TARGET = testWorkoutGame3DHealth

INCLUDEPATH += ../../../src/Train

SOURCES += testWorkoutGame3DHealth.cpp \
           ../../../src/Train/WorkoutGame3DHealth.cpp

HEADERS += ../../../src/Train/WorkoutGame3DHealth.h

include(../../unittests.pri)
