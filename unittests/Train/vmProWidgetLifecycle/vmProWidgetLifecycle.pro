QT += core gui widgets testlib bluetooth

TEMPLATE = app
TARGET = tst_vmProWidgetLifecycle

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testVMProWidgetLifecycle.cpp \
          VMProWidgetTestStubs.cpp

HEADERS = ../../../src/Train/VMProWidget.h

INCLUDEPATH += ../../../src \
               ../../../src/Train

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
