TEMPLATE = aux

win32:SECTION_FLAGS_PYTHON = python
else:SECTION_FLAGS_PYTHON = python3

check.commands = $$SECTION_FLAGS_PYTHON $$shell_quote($$PWD/testLinkerSectionFlags.py)
check.CONFIG += phony
QMAKE_EXTRA_TARGETS += check
