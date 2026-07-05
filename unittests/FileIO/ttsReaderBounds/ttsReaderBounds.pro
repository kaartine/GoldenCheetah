QT += core testlib

TEMPLATE = app
TARGET = tst_ttsReaderBounds

include(../../unittests.pri)

CONFIG += console testcase c++17

SOURCES = testTTSReaderBounds.cpp \
          ../../../src/FileIO/TTSReader.cpp \
          ../../../src/FileIO/LocationInterpolation.cpp \
          ../../../src/Metrics/BlinnSolver.cpp

INCLUDEPATH += ../../../src/FileIO \
               ../../../src/Metrics \
               ../../../qwt/src

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
