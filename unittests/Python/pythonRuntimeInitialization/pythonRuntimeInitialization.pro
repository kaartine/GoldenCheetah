TEMPLATE = app
TARGET = tst_realPythonRuntimeInitialization
CONFIG += console c++17
CONFIG -= app_bundle
QT -= core gui

isEmpty(PYTHONINCLUDES): error("Pass PYTHONINCLUDES for the embedded Python build")
isEmpty(PYTHONLIBS): error("Pass PYTHONLIBS for the embedded Python build")

INCLUDEPATH += $$replace(PYTHONINCLUDES, ^-I, ) \
               $$PWD/../../../src
LIBS += $$PYTHONLIBS

SOURCES += realPythonRuntimeInitialization.cpp
HEADERS += $$PWD/../../../src/Python/PythonRuntimeInitializer.h \
           $$PWD/../../../src/Python/PythonRuntimeFinalizer.h \
           $$PWD/../../../src/Python/PythonPathAppender.h
