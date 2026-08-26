QT += core testlib
CONFIG += testcase console c++17
TEMPLATE = app
TARGET = testWorkoutGameAudio

INCLUDEPATH += ../../../src/Train

SOURCES += testWorkoutGameAudio.cpp \
           ../../../src/Train/WorkoutGameAudioEvents.cpp

HEADERS += ../../../src/Train/WorkoutGameAudioEvents.h

RESOURCES += ../../../src/Resources/workout-game-assets.qrc

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CFLAGS += -fsanitize=address,undefined \
                    -fno-omit-frame-pointer \
                    -fno-sanitize-recover=all
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
                      -fno-omit-frame-pointer \
                      -fno-sanitize-recover=all
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
