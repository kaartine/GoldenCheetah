TEMPLATE = aux

win32:CI_PYTHON = python
else:CI_PYTHON = python3

check.commands = $$CI_PYTHON $$shell_quote($$PWD/testCiTestRunner.py)
check.commands += && $$CI_PYTHON $$shell_quote($$PWD/testMemcheckRunner.py)
check.commands += && $$CI_PYTHON $$shell_quote($$PWD/../../Gui/preReleaseUi/test_ui_memcheck.py)
check.CONFIG += phony
QMAKE_EXTRA_TARGETS += check
