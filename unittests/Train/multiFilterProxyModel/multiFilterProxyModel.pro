QT += core gui testlib

TEMPLATE = app
TARGET = testMultiFilterProxyModel

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testMultiFilterProxyModel.cpp \
          ../../../src/Train/ModelFilter.cpp \
          ../../../src/Train/MultiFilterProxyModel.cpp

HEADERS = ../../../src/Train/ModelFilter.h \
          ../../../src/Train/MultiFilterProxyModel.h

INCLUDEPATH += ../../../src \
               ../../../src/Train

include(../../section-gc.prf)

sanitize:!tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
