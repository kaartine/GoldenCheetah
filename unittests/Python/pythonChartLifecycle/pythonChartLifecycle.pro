QT += core gui widgets testlib concurrent core5compat

TEMPLATE = app
TARGET = tst_pythonChartLifecycle
CONFIG += console testcase c++17 release
CONFIG -= debug

include(../../unittests.pri)

SOURCES = testPythonChartLifecycle.cpp \
          $$PWD/../../../src/Core/Specification.cpp \
          $$PWD/../../../src/Core/TimeUtils.cpp \
          $$PWD/../../../src/Python/PythonExecutionGate.cpp \
          $$PWD/../../../src/Python/PythonChartRunState.cpp \
          $$PWD/../../../src/Python/PythonChartRunner.cpp \
          $$PWD/../../../src/Python/PythonChartOwner.cpp

HEADERS = $$PWD/../../../src/Python/PythonExecutionGate.h \
          $$PWD/../../../src/Python/PythonChartRunState.h \
          $$PWD/../../../src/Python/PythonChartRunner.h \
          $$PWD/../../../src/Python/PythonChartOwner.h \
          $$PWD/../../../src/Python/PythonEmbed.h

INCLUDEPATH += $$PWD/../../../src \
               $$PWD/../../../src/ANT \
               $$PWD/../../../src/Charts \
               $$PWD/../../../src/Core \
               $$PWD/../../../src/FileIO \
               $$PWD/../../../src/Gui \
               $$PWD/../../../src/Metrics \
               $$PWD/../../../src/Planning \
               $$PWD/../../../src/Python \
               $$PWD/../../../src/Train \
               $$PWD/../../../qwt/src \
               $$PWD/../../../contrib/boost

QMAKE_CXXFLAGS += -ffunction-sections -fdata-sections
QMAKE_LFLAGS += -Wl,--gc-sections

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
