QT += testlib widgets svg

TEMPLATE = app
TARGET = tst_indendPlotMarkerMatrix

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug app_bundle

DEFINES += GC_INDEND_PLOT_MARKER_TEST

SOURCES = testIndendPlotMarkerMatrix.cpp \
          ../../../src/Charts/IndendPlotMarker.cpp

HEADERS = ../../../src/Charts/IndendPlotMarker.h

INCLUDEPATH += ../../../src \
               ../../../src/Charts \
               ../../../qwt/src

isEmpty(QWT_LIB_DIR) {
    QWT_LIB_DIR = $${OUT_PWD}/../../../qwt/lib
}
LIBS += -L$${QWT_LIB_DIR} -lqwt
QMAKE_RPATHDIR += $${QWT_LIB_DIR}

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
