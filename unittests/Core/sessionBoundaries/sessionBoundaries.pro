QT += core gui widgets testlib

greaterThan(QT_MAJOR_VERSION, 5): QT += core5compat

TEMPLATE = app
TARGET = tst_sessionBoundaries

include(../../unittests.pri)

CONFIG += console testcase c++17
CONFIG -= app_bundle
DEFINES += GC_CONTEXT_SESSION_INTEGRATION_TEST

SOURCES = testSessionBoundaries.cpp \
          SessionBoundaryLinkStubs.cpp \
          ../../../src/Core/AthleteSession.cpp \
          ../../../src/Core/Context.cpp \
          ../../../src/Core/TrainingSession.cpp

HEADERS = ../../../src/Core/AthleteSession.h \
          ../../../src/Core/Context.h \
          ../../../src/Core/RideItem.h \
          ../../../src/Core/TrainingSession.h \
          ../../../src/Core/SessionServices.h

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

sanitize:!tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}

tsan:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=thread \
                      -fno-omit-frame-pointer \
                      -fno-pie \
                      -O1 \
                      -g
    QMAKE_LFLAGS += -fsanitize=thread \
                    -no-pie
}
