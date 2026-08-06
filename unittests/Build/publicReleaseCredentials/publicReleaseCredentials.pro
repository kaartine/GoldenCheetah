TEMPLATE = aux

check.commands = bash $$PWD/testPublicReleaseCredentials.sh
check.CONFIG += phony
QMAKE_EXTRA_TARGETS += check
