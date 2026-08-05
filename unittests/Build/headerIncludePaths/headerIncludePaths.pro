TEMPLATE = aux

win32:HEADER_INCLUDE_TEST_PYTHON = python
else:HEADER_INCLUDE_TEST_PYTHON = python3

check.commands = $$HEADER_INCLUDE_TEST_PYTHON $$shell_quote($$PWD/testHeaderIncludePaths.py)
QMAKE_EXTRA_TARGETS += check
