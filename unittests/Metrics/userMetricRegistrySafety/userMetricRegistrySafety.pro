QT += core gui widgets testlib xml sql network svg concurrent serialport \
      multimedia multimediawidgets webenginecore webenginewidgets webchannel \
      positioning webenginequick charts openglwidgets core5compat

TEMPLATE = app
TARGET = tst_userMetricRegistrySafety

include(../../unittests.pri)

CONFIG += console testcase c++17 release
CONFIG -= debug app_bundle

DEFINES += GC_USER_METRIC_REGISTRY_SAFETY_TEST

SOURCES = testUserMetricRegistrySafety.cpp \
          UserMetricRegistrySafetyTestStubs.cpp \
          ../../../src/Metrics/RideMetric.cpp \
          ../../../src/Metrics/UserMetric.cpp

HEADERS = UserMetricRegistrySafetyTestTypes.h \
          ../../../src/Core/Context.h \
          ../../../src/Core/DataFilter.h \
          ../../../src/Core/RideItem.h \
          ../../../src/Metrics/PDModel.h

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
               ../../../qwt/src \
               $${GSL_INCLUDES}

LIBS += $${GSL_LIBS}

include(../../section-gc.prf)

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
    QMAKE_LFLAGS += -fsanitize=thread -no-pie
}
