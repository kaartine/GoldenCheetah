QT += core testlib

TEMPLATE = app
TARGET = tst_openDataCaptureUtils

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testOpenDataCaptureUtils.cpp \
          ../../../src/Cloud/OpenDataCaptureUtils.cpp

HEADERS = ../../../src/Cloud/OpenDataCaptureUtils.h

INCLUDEPATH += ../../../src

sanitize:!tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}

linux:tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=thread \
                      -fno-omit-frame-pointer \
                      -fno-pie \
                      -O1 \
                      -g
    QMAKE_LFLAGS += -fsanitize=thread \
                    -no-pie
}
