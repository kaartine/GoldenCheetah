QT += core gui widgets testlib

TEMPLATE = app
TARGET = usbXpressSafety

CONFIG += console testcase c++17
CONFIG -= app_bundle

DEFINES += WIN32 GC_HAVE_LIBUSB GC_HAVE_USBXPRESS

QMAKE_CXXFLAGS += -I$$[QT_INSTALL_HEADERS] \
                  -include $$PWD/QtPlatformShim.h

SOURCES = testUSBXpressSafety.cpp \
          FakeSiUSBXp.cpp \
          ../antThreadSafety/FakeLibUsb.cpp \
          ../antThreadSafety/AntThreadSafetyTestStubs.cpp \
          ../../../src/ANT/ANT.cpp \
          ../../../src/ANT/ANTChannel.cpp \
          ../../../src/ANT/ANTMessage.cpp \
          ../../../src/Train/RealtimeData.cpp \
          ../../../src/Train/CalibrationData.cpp \
          ../../../src/Train/USBXpress.cpp

HEADERS = windows.h \
          winbase.h \
          SiUSBXp.h \
          FakeSiUSBXp.h \
          QtPlatformShim.h \
          ../antThreadSafety/LibUsb.h \
          ../antThreadSafety/GoldenCheetah.h \
          ../antThreadSafety/DeviceConfiguration.h \
          ../antThreadSafety/Settings.h \
          ../antThreadSafety/RideFile.h \
          ../antThreadSafety/TrainSidebar.h \
          ../../../src/ANT/ANT.h \
          ../../../src/ANT/ANTChannel.h

INCLUDEPATH += $$PWD \
               $$PWD/../antThreadSafety \
               $$PWD/../../../src/ANT \
               $$PWD/../../../src/Train

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
