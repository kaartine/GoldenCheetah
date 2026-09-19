TEMPLATE = aux

win32:SOURCE_MODULE_DEPENDENCY_PYTHON = python
else:SOURCE_MODULE_DEPENDENCY_PYTHON = python3

check.commands = $$SOURCE_MODULE_DEPENDENCY_PYTHON $$shell_quote($$PWD/testSourceModuleDependencies.py)
QMAKE_EXTRA_TARGETS += check
