QT += core gui testlib

TEMPLATE = app
TARGET = tst_voronoiSafety

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug app_bundle

SOURCES = testVoronoiSafety.cpp \
          ../../../contrib/voronoi/Voronoi.cpp

INCLUDEPATH += ../../../contrib/voronoi

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
