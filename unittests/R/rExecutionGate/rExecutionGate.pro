QT += core testlib

TEMPLATE = app
TARGET = tst_rExecutionGate
CONFIG += console testcase c++17 release
CONFIG -= debug

include(../../unittests.pri)

GC_TEST_SOURCE_ROOT = $$clean_path($$_PRO_FILE_PWD_/../../..)
DEFINES += GC_TEST_SOURCE_ROOT=\\\"$${GC_TEST_SOURCE_ROOT}\\\"

SOURCES = testRExecutionGate.cpp \
          $$PWD/../../../src/R/RExecutionGate.cpp

HEADERS = $$PWD/../../../src/R/RExecutionGate.h \
          $$PWD/../../../src/R/RDeferredUiWork.h \
          $$PWD/../../../src/R/RProtectionScope.h \
          $$PWD/../../../src/Charts/RConsolePromptPolicy.h \
          $$PWD/../../../src/Charts/RWidgetExecutionGuard.h

INCLUDEPATH += $$PWD/../../../src

sanitize {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize=vptr \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}

thread_sanitize {
    QMAKE_CXXFLAGS += -fsanitize=thread \
                      -fno-omit-frame-pointer \
                      -fno-pie \
                      -O1 -g
    QMAKE_LFLAGS += -fsanitize=thread -no-pie
}
