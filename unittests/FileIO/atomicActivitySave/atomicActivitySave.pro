QT += core gui widgets testlib xml sql network svg concurrent serialport \
      multimedia multimediawidgets webenginecore webenginewidgets webchannel \
      positioning webenginequick charts openglwidgets core5compat

TEMPLATE = app
TARGET = tst_atomicActivitySave

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug
DEFINES += GC_LINKED_ACTIVITY_SAVE_TEST_HOOKS \
           GC_ANCHORED_FILESYSTEM_TEST_HOOKS \
           GC_ATOMIC_FILE_WRITER_TEST_HOOKS

SOURCES = testAtomicActivitySave.cpp \
          ApplicationSaveTestStubs.cpp \
          ../../../src/Core/LinkedActivitySaveJournal.cpp \
          ../../../src/Core/RideCache.cpp \
          ../../../src/FileIO/AnchoredFileSystem.cpp \
          ../../../src/FileIO/RideFile.cpp \
          ../../../src/FileIO/RideFileCRC.cpp \
          ../../../src/FileIO/RideFileCommand.cpp \
          JsonRideFileTestStubs.cpp \
          ../../../src/Gui/SaveDialogs.cpp

HEADERS = ../../../src/Core/RideItem.h \
          ../../../src/Core/LinkedActivitySaveJournal.h \
          ../../../src/FileIO/AnchoredFileSystem.h \
          ../../../src/FileIO/RideFile.h \
          ../../../src/FileIO/RideFileCommand.h \
          ../../../src/FileIO/JsonRideFile.h \
          ../../../src/Gui/SaveDialogs.h

YACCSOURCES = ../../../src/FileIO/JsonRideFile.y
LEXSOURCES = ../../../src/FileIO/JsonRideFile.l

INCLUDEPATH += ../../../src \
               ../../../src/ANT \
               ../../../src/Charts \
               ../../../src/Cloud \
               ../../../src/Core \
               ../../../src/FileIO \
               ../../../src/Gui \
               ../../../src/Metrics \
               ../../../src/Planning \
               ../../../src/Train \
               ../../../qwt/src \
               ../../../contrib/qzip \
               $${GSL_INCLUDES} \
               $${ZLIB_INCLUDES}

LIBS += $${GSL_LIBS}
win32:LIBS += -ladvapi32

isEmpty(ZLIB_LIBS) {
    LIBS += -lz
} else {
    LIBS += $${ZLIB_LIBS}
}

unix {
    QMAKE_CXXFLAGS += -ffunction-sections -fdata-sections
}

unix:!macx {
    QMAKE_LFLAGS += -Wl,--gc-sections
}

macx {
    QMAKE_LFLAGS += -Wl,-dead_strip
}

msvc {
    QMAKE_CXXFLAGS += /Gy /Gw
    QMAKE_LFLAGS += /OPT:REF
}

sanitize:!tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}

linux:tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=thread \
                      -fno-omit-frame-pointer \
                      -O1 \
                      -g
    QMAKE_LFLAGS += -fsanitize=thread
    QMAKE_CXXFLAGS += -fno-pie
    QMAKE_LFLAGS += -no-pie
}
