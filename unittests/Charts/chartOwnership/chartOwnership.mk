# Include AFTER the exact full application's generated Makefile. This target
# deliberately has no application-object prerequisites: it must never rebuild
# or modify the supplied cache. See README.md for its explicit prerequisites.
CHART_FIXTURE_SOURCE := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
CHART_FIXTURE_QT := $(shell $(QMAKE) -query QT_INSTALL_PREFIX)
CHART_FIXTURE_OUT ?=
CHART_FIXTURE_MAIN ?= ../../source/src/Core/main.cpp
CHART_FIXTURE_LIBS ?= $(LIBS)
CHART_FIXTURE_OBJECTS ?= $(filter-out main.o,$(OBJECTS))
CHART_FIXTURE_BINARY ?= $(CHART_FIXTURE_OUT)/tst_chartOwnership
CHART_FIXTURE_LINK = $(LINK) $(LFLAGS) -o $(CHART_FIXTURE_BINARY) $(CHART_FIXTURE_OBJECTS) $(CHART_FIXTURE_OUT)/renamed-main.o $(CHART_FIXTURE_OUT)/testChartOwnership.o $(OBJCOMP) $(CHART_FIXTURE_QT)/lib/libQt6Test.so $(CHART_FIXTURE_LIBS)

.PHONY: chart-ownership
chart-ownership:
	@test -n "$(CHART_FIXTURE_OUT)" && test -d "$(CHART_FIXTURE_OUT)"
	$(CHART_FIXTURE_QT)/libexec/moc $(DEFINES) $(INCPATH) -I$(CHART_FIXTURE_QT)/include/QtTest $(CHART_FIXTURE_SOURCE)/testChartOwnership.cpp -o $(CHART_FIXTURE_OUT)/testChartOwnership.moc
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -DQT_TESTLIB_LIB -I$(CHART_FIXTURE_QT)/include/QtTest -I$(CHART_FIXTURE_OUT) $(CHART_FIXTURE_SOURCE)/testChartOwnership.cpp -o $(CHART_FIXTURE_OUT)/testChartOwnership.o
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -Dmain=gc_unused_application_main $(CHART_FIXTURE_MAIN) -o $(CHART_FIXTURE_OUT)/renamed-main.o
	$(CHART_FIXTURE_LINK)

# Optional differential check with explicitly selected old production objects.
# The caller must use a different CHART_FIXTURE_BINARY to preserve the fixed one.
.PHONY: chart-ownership-link
chart-ownership-link:
	@test -n "$(CHART_FIXTURE_OUT)" && test -d "$(CHART_FIXTURE_OUT)"
	$(CHART_FIXTURE_LINK)
