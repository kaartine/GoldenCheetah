TEMPLATE = aux

check.commands = bash $$PWD/testAppImagePackaging.sh
check.CONFIG += phony
QMAKE_EXTRA_TARGETS += check
