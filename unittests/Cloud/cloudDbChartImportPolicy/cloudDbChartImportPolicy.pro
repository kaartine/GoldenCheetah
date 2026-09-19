QT += testlib core widgets
CONFIG += c++17

TARGET = testCloudDbChartImportPolicy

SOURCES = testCloudDbChartImportPolicy.cpp \
          ../../../src/Cloud/CloudDBChartImportPolicy.cpp

HEADERS = ../../../src/Cloud/CloudDBChartImportPolicy.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
