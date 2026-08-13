QT += core widgets testlib

TEMPLATE = app
TARGET = tst_libraryParserSerialize

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

DEFINES += GC_LIBRARY_PARSER_SERIALIZE_TEST_HOOKS

SOURCES = testLibraryParserSerialize.cpp \
          ../../../src/Train/LibraryParser.cpp

HEADERS = LibraryParserSerializeTestStubs.h

INCLUDEPATH += . \
               ../../../src \
               ../../../src/Train

sanitize:!tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}

tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=thread \
                      -fno-omit-frame-pointer \
                      -O1 \
                      -g
    QMAKE_LFLAGS += -fsanitize=thread
    linux {
        QMAKE_CXXFLAGS += -fno-pie
        QMAKE_LFLAGS += -no-pie
    }
}
