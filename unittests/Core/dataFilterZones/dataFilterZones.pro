QT += core gui testlib xml sql network svg widgets concurrent serialport \
      multimedia multimediawidgets webenginecore webenginewidgets webchannel \
      positioning webenginequick core5compat

TEMPLATE = app
TARGET = tst_dataFilterZones

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug app_bundle

SOURCES = testDataFilterZones.cpp \
          DataFilterParserTestStubs.cpp \
          ../../../src/Core/DataFilterSafety.cpp \
          ../../../src/Core/DataFilterZones.cpp

YACCSOURCES += ../../../src/Core/DataFilter.y
LEXSOURCES += ../../../src/Core/DataFilter.l

INCLUDEPATH += ../../../src/ANT \
               ../../../src/Train \
               ../../../src/FileIO \
               ../../../src/Cloud \
               ../../../src/Charts \
               ../../../src/Metrics \
               ../../../src/Gui \
               ../../../src/Core \
               ../../../src/Planning \
               ../../../qwt/src \
               ../../../contrib/qxt/src \
               ../../../contrib/qtsolutions/json \
               ../../../contrib/qtsolutions/qwtcurve \
               ../../../contrib/qtsolutions/flowlayout \
               ../../../contrib/lmfit \
               ../../../contrib/boost \
               ../../../contrib/kmeans \
               ../../../contrib/voronoi

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
