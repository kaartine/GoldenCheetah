QT += widgets testlib

TEMPLATE = app
TARGET = tst_cacheWriteWarning

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testCacheWriteWarning.cpp \
          ../../../src/Gui/CacheWriteWarning.cpp

HEADERS = ../../../src/Gui/CacheWriteWarning.h

INCLUDEPATH += ../../../src/Gui

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
