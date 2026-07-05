QT += testlib widgets core5compat

DEFINES += GC_ANT_BURST_TEST

SOURCES = testAntBurstBounds.cpp \
          ../../../src/ANT/ANT.cpp \
          ../../../src/ANT/ANTChannel.cpp \
          ../../../src/ANT/ANTMessage.cpp

INCLUDEPATH += $$PWD/../../../src/ANT \
               $$PWD/../../../src/Train \
               $$PWD/../../../src/FileIO \
               $$PWD/../../../src/Cloud \
               $$PWD/../../../src/Charts \
               $$PWD/../../../src/Metrics \
               $$PWD/../../../src/Gui \
               $$PWD/../../../src/Core \
               $$PWD/../../../src/Planning

GC_OBJS = RealtimeData \
          CalibrationData \
          moc_ANT \
          moc_ANTChannel

include(../../unittests.pri)

sanitize:!msvc {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
