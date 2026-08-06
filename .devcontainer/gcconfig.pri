# Minimal development configuration for the container build.
CONFIG += debug

LIBZ_INCLUDE = /usr/include
LIBZ_LIBS = -lz

GSL_INCLUDES = /usr/include
GSL_LIBS = -lgsl -lgslcblas -lm

# src.pro assumes an in-tree Qwt build; this development setup builds out of tree.
LIBS += -L$${OUT_PWD}/../qwt/lib

QMAKE_LEX = flex
QMAKE_YACC = bison
QMAKE_MOVE = cp

QMAKE_CXXFLAGS += -O0 -g

# Video playback is unrelated to the BLE proof of concept.
DEFINES += GC_VIDEO_NONE

# A source build needs its own Strava application credentials. Put the client
# secret in the ignored src/Core/GeneratedSecrets.h, never in qmake DEFINES.
# DEFINES += GC_STRAVA_CLIENT_ID=\\\"your_client_id\\\"
