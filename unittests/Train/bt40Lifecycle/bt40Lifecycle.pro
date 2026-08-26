QT += core gui widgets testlib bluetooth network xml sql svg concurrent serialport \
      multimedia multimediawidgets webenginecore webenginewidgets webchannel \
      positioning webenginequick charts openglwidgets

greaterThan(QT_MAJOR_VERSION, 5): QT += core5compat

TEMPLATE = app
TARGET = tst_bt40Lifecycle

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug

SOURCES = testBt40Lifecycle.cpp \
          Bt40LifecycleTestStubs.cpp \
          FakeBluetoothDiscovery.cpp \
          FakeLowEnergyController.cpp \
          ../../../src/Train/RealtimeData.cpp \
          ../../../src/Train/RealtimeController.cpp \
          ../../../src/Train/WorkoutRideTargetPlanner.cpp \
          ../../../src/Train/BT40Controller.cpp \
          ../../../src/Train/BT40Device.cpp \
          ../../../src/Train/BluetoothTrainerCapabilities.cpp \
          ../../../src/Train/BluetoothTelemetryRouter.cpp \
          ../../../src/Train/CalibrationData.cpp \
          ../../../src/Train/Ftms.cpp \
          ../../../src/Train/KurtInRide.cpp \
          ../../../src/Train/KurtSmartControl.cpp

HEADERS = ../../../src/Train/RealtimeController.h \
          ../../../src/Train/WorkoutRideTargetPlanner.h \
          ../../../src/Train/BT40Controller.h \
          ../../../src/Train/BT40Device.h \
          ../../../src/Train/BluetoothTrainerCapabilities.h \
          ../../../src/Train/VMProWidget.h \
          QBluetoothDeviceDiscoveryAgent \
          QBluetoothLocalDevice \
          QLowEnergyCharacteristic \
          QLowEnergyController \
          QLowEnergyDescriptor \
          QLowEnergyService

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
