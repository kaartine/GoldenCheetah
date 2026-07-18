QT += core testlib concurrent

TEMPLATE = app
TARGET = tst_mergeActivityAlignment

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testMergeActivityAlignment.cpp \
          ../../../src/Gui/MergeActivityAlignment.cpp

HEADERS = ../../../src/Gui/MergeActivityAlignment.h

INCLUDEPATH += ../../../src/Gui \
               $${GSL_INCLUDES}

LIBS += $${GSL_LIBS}

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}

leakcheck:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=leak -fno-omit-frame-pointer
    QMAKE_LFLAGS += -fsanitize=leak
}
