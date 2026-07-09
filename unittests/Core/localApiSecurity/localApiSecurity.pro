QT += testlib core network
CONFIG += c++17

TARGET = tst_localApiSecurity

SOURCES += testLocalApiSecurity.cpp
SOURCES += ../../../src/Core/LocalApiSecurityPolicy.cpp

HEADERS += ../../../src/Core/LocalApiSecurityPolicy.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined
    QMAKE_CXXFLAGS += -fno-omit-frame-pointer
    QMAKE_CXXFLAGS += -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
