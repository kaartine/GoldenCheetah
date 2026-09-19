TEMPLATE = app
TARGET = tst_realPythonRuntimeShutdown
CONFIG += console c++17
CONFIG -= app_bundle
QT -= core gui

isEmpty(PYTHONINCLUDES): error("Pass PYTHONINCLUDES for the embedded Python build")
isEmpty(PYTHONLIBS): error("Pass PYTHONLIBS for the embedded Python build")

INCLUDEPATH += $$replace(PYTHONINCLUDES, ^-I, ) \
               $$PWD/../../../src
LIBS += $$PYTHONLIBS

SOURCES += realPythonRuntimeShutdown.cpp
HEADERS += $$PWD/../../../src/Python/PythonRuntimeFinalizer.h
