QT += core gui widgets testlib
greaterThan(QT_MAJOR_VERSION, 5): QT += core5compat

TEMPLATE = app
TARGET = tst_rideCacheSaveSnapshot

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testRideCacheSaveSnapshot.cpp \
          RideCacheSaveCaptureTestStubs.cpp \
          ../../../src/Core/RideCacheBackgroundSaver.cpp \
          ../../../src/Core/RideCacheSaveCapture.cpp \
          ../../../src/Core/RideCacheSaveSnapshot.cpp \
          ../../../src/Core/RideCachePersistence.cpp

HEADERS = ../../../src/Core/RideCacheBackgroundSaver.h \
          ../../../src/Core/RideCacheSaveCapture.h \
          ../../../src/Core/RideCacheSaveSnapshot.h \
          ../../../src/Core/RideCacheStartup.h \
          ../../../src/Core/IntervalItem.h \
          ../../../src/Core/RideItem.h \
          ../../../src/Core/RideCachePersistence.h \
          ../../../src/FileIO/AtomicFileWriter.h

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
               ../../../qwt/src

!msvc {
    QMAKE_CXXFLAGS += -ffunction-sections -fdata-sections
}
linux|win32-g++ {
    QMAKE_LFLAGS += -Wl,--gc-sections
}
macx {
    QMAKE_LFLAGS += -Wl,-dead_strip
}

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
