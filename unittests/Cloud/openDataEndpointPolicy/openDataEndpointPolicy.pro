QT += testlib core network
CONFIG += c++17

TARGET = testOpenDataEndpointPolicy

SOURCES = testOpenDataEndpointPolicy.cpp \
          ../../../src/Cloud/OpenDataEndpointPolicy.cpp

HEADERS = ../../../src/Cloud/OpenDataEndpointPolicy.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
