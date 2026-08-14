QT += core network testlib

TEMPLATE = app
TARGET = tst_openDataExport

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

DEFINES += GC_OPEN_DATA_TEMPORARY_ARCHIVE_TEST_HOOKS

SOURCES = testOpenDataExport.cpp \
          ../../../src/Cloud/OpenDataExport.cpp \
          ../../../src/Cloud/OpenDataTemporaryArchive.cpp \
          ../../../src/Cloud/OpenDataUploadWorker.cpp \
          ../../../src/Cloud/NetworkReplyWait.cpp \
          ../../../contrib/qzip/zip.cpp

HEADERS = ../../../src/Cloud/OpenDataExport.h \
          ../../../src/Cloud/OpenDataTemporaryArchive.h \
          ../../../src/Cloud/OpenDataUploadWorker.h \
          ../../../src/Cloud/NetworkReplyWait.h

INCLUDEPATH += ../../../src \
               ../../../contrib/qzip \
               $${LIBZ_INCLUDE}

include(../../zlib-link.prf)

win32:LIBS += -ladvapi32

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
