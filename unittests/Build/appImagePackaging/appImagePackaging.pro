TEMPLATE = aux

check.commands = python3 $$PWD/testReleaseHardening.py
check.commands += && bash $$PWD/testAppImagePackaging.sh
check.CONFIG += phony
QMAKE_EXTRA_TARGETS += check
