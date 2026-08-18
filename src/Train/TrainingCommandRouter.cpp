/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "TrainingCommandRouter.h"

#include <QAbstractSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>

TrainingCommand TrainingCommandRouter::commandForKey(
        int key,
        Qt::KeyboardModifiers modifiers,
        bool autoRepeat)
{
    constexpr Qt::KeyboardModifiers reservedModifiers =
            Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;
    if (autoRepeat || (modifiers & reservedModifiers)) {
        return TrainingCommand::None;
    }

    switch (key) {
    case Qt::Key_Space:
        return TrainingCommand::ToggleStartPause;
    case Qt::Key_Escape:
        return TrainingCommand::RequestStop;
    case Qt::Key_Up:
    case Qt::Key_W:
        return TrainingCommand::ShiftUp;
    case Qt::Key_Down:
    case Qt::Key_S:
        return TrainingCommand::ShiftDown;
    default:
        return TrainingCommand::None;
    }
}

bool TrainingCommandRouter::shouldPreserveFocusedInput(
        const QWidget *focus,
        bool modalDialogActive)
{
    if (modalDialogActive) return true;
    if (const QComboBox *combo = qobject_cast<const QComboBox *>(focus)) {
        return combo->isEditable();
    }
    return qobject_cast<const QLineEdit *>(focus)
            || qobject_cast<const QTextEdit *>(focus)
            || qobject_cast<const QPlainTextEdit *>(focus)
            || qobject_cast<const QAbstractSpinBox *>(focus);
}
