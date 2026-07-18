QT += core testlib

TEMPLATE = app
TARGET = tst_rideImportBatch

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testRideImportBatch.cpp

HEADERS = ../../../src/Core/RideCacheBulkMerge.h \
          ../../../src/Gui/RideImportRideStore.h

INCLUDEPATH += ../../../src \
               ../../../src/Core \
               ../../../src/FileIO \
               ../../../src/Gui

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}

leakcheck:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=leak -fno-omit-frame-pointer
    QMAKE_LFLAGS += -fsanitize=leak
}
