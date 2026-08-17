QT += core testlib bluetooth
CONFIG += c++17

TARGET = testTrainingDataGenerator

SOURCES = testTrainingDataGenerator.cpp \
          ../../../src/Train/TrainingDataGenerator.cpp \
          ../../../src/Train/DeviceTypes.cpp

HEADERS = ../../../src/Train/TrainingDataGenerator.h \
          ../../../src/Train/TrainingDeviceWizardRouting.h \
          ../../../src/Train/DeviceTypes.h \
          ../../../src/Train/BluetoothDeviceTypes.h

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
