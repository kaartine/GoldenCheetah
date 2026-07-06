QT += core testlib

TEMPLATE = app
TARGET = tst_splitActivitySave

CONFIG += console testcase c++17 release
CONFIG -= debug debug_and_release

SOURCES = testSplitActivitySave.cpp

exists(../../../src/Gui/SplitActivitySave.cpp) {
    SOURCES += ../../../src/Gui/SplitActivitySave.cpp
}

include(../../unittests.pri)

INCLUDEPATH += ../../../src/FileIO \
               ../../../src/Gui

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
