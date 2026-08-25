TEMPLATE = aux

win32:WORKOUT_GAME_ASSET_PYTHON = python
else:WORKOUT_GAME_ASSET_PYTHON = python3

check.commands = $$WORKOUT_GAME_ASSET_PYTHON $$shell_quote($$PWD/testWorkoutGameAssets.py)
check.CONFIG += phony
QMAKE_EXTRA_TARGETS += check
