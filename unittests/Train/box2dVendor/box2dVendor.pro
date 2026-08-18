QT += core testlib
CONFIG += c++17

TARGET = testBox2DVendor

SOURCES = testBox2DVendor.cpp

BOX2D_ROOT = $$clean_path($$PWD/../../../vendor/box2d-3.1.1)
include($$BOX2D_ROOT/box2d.pri)

sanitize:!msvc {
    QMAKE_CFLAGS += -fsanitize=address,undefined \
                    -fno-omit-frame-pointer \
                    -fno-sanitize-recover=all
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}

include(../../unittests.pri)
