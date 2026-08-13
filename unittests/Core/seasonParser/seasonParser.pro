QT += core core5compat testlib widgets xml

TEMPLATE = app
TARGET = tst_seasonParser

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testSeasonParser.cpp \
          ../../../src/Core/Seasons.cpp \
          ../../../src/Core/Season.cpp \
          ../../../src/Core/Utils.cpp \
          ../../../src/FileIO/AnchoredFileSystem.cpp

HEADERS = ../../../src/Core/Seasons.h \
          ../../../src/Core/Season.h \
          ../../../src/FileIO/AnchoredFileSystem.h

INCLUDEPATH += ../../../src \
               ../../../src/Charts \
               ../../../src/Core \
               ../../../src/FileIO \
               ../../../src/Metrics

win32:LIBS += -ladvapi32

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}

include(../../unittests.pri)
