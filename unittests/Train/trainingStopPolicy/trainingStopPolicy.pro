QT += testlib core

SOURCES = testTrainingStopPolicy.cpp

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
