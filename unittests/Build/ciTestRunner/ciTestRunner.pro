TEMPLATE = aux

win32:CI_PYTHON = python
else:CI_PYTHON = python3

check.commands = $$CI_PYTHON $$shell_quote($$PWD/testCiTestRunner.py)
check.CONFIG += phony
QMAKE_EXTRA_TARGETS += check
