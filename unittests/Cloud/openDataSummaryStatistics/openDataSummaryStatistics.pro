QT += core testlib

TEMPLATE = app
TARGET = tst_openDataSummaryStatistics

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testOpenDataSummaryStatistics.cpp \
          ../../../src/Cloud/OpenDataSummaryStatistics.cpp

HEADERS = ../../../src/Cloud/OpenDataSummaryStatistics.h

INCLUDEPATH += ../../../src \
               ../../../src/Cloud

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
