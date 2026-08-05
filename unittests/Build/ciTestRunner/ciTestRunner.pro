TEMPLATE = aux

check.commands = bash $$PWD/testCiTestRunner.sh
check.CONFIG += phony
QMAKE_EXTRA_TARGETS += check
