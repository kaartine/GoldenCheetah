QT += core gui widgets testlib

TEMPLATE = app
TARGET = antThreadSafety

CONFIG += console testcase c++17

DEFINES += GC_HAVE_LIBUSB

SOURCES = testAntThreadSafety.cpp \
          FakeLibUsb.cpp \
          AntThreadSafetyTestStubs.cpp \
          ../../../src/ANT/ANT.cpp \
          ../../../src/ANT/ANTChannel.cpp \
          ../../../src/ANT/ANTMessage.cpp \
          ../../../src/Train/RealtimeData.cpp \
          ../../../src/Train/CalibrationData.cpp

HEADERS = LibUsb.h \
          GoldenCheetah.h \
          DeviceConfiguration.h \
          Settings.h \
          RideFile.h \
          TrainSidebar.h \
          ../../../src/ANT/ANT.h \
          ../../../src/ANT/ANTChannel.h

INCLUDEPATH += $$PWD \
               $$PWD/../../../src/ANT \
               $$PWD/../../../src/Train

include(../../unittests.pri)

# ThreadSanitizer cannot be combined with the ASan/UBSan configuration.
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
