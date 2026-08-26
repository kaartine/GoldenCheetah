QT += core gui widgets testlib bluetooth core5compat
CONFIG += c++17

TARGET = testTrainingDataGenerator

INCLUDEPATH += ../../../src/Charts ../../../src/Core ../../../src/FileIO \
               ../../../src/Gui ../../../src/Metrics ../../../src/Planning \
               ../../../src/Train ../../../qwt/src

SOURCES = testTrainingDataGenerator.cpp \
          ../../../src/Train/TrainingDataGenerator.cpp \
          ../../../src/Train/TrainingDataGeneratorTargetRouting.cpp \
          ../../../src/Train/RealtimeData.cpp \
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
