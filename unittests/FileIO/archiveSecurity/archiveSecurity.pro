QT += testlib core
CONFIG += c++17

SOURCES = testArchiveSecurity.cpp \
          ../../../contrib/qzip/zip.cpp \
          ../../../src/FileIO/ArchiveFile.cpp \
          ../../../src/FileIO/CompressedActivityFile.cpp

include(../../unittests.pri)

INCLUDEPATH += ../../../src ../../../contrib/qzip $${LIBZ_INCLUDE}

LIBS += $${LIBZ_LIBS}

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
