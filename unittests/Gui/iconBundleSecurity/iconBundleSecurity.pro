QT += testlib core gui widgets svg network sql xml core5compat
CONFIG += c++17
DEFINES += GC_ICON_BUNDLE_SECURITY_TEST

SOURCES = testIconBundleSecurity.cpp \
          ../../../src/Gui/IconManager.cpp \
          ../../../contrib/qzip/zip.cpp

include(../../unittests.pri)

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
               ../../../contrib/qxt/src \
               ../../../contrib/qzip \
               $${LIBZ_INCLUDE}

LIBS += $${LIBZ_LIBS}

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
