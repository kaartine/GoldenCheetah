QT += testlib core
CONFIG += c++17

SOURCES = testArchiveSecurity.cpp \
          ../../../contrib/qzip/zip.cpp \
          ../../../src/FileIO/ArchiveFile.cpp

include(../../unittests.pri)

INCLUDEPATH += ../../../src ../../../contrib/qzip $${LIBZ_INCLUDE}

LIBS += $${LIBZ_LIBS}
