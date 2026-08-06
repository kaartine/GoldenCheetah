/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_OAUTH_DIALOG_MESSAGE_GUARD_H
#define GC_OAUTH_DIALOG_MESSAGE_GUARD_H

#include <QMessageBox>
#include <QString>

#include <functional>

class QDialog;

namespace OAuthDialogMessageGuard {

using MessageExecutor = std::function<void(QMessageBox &)>;

bool showAndReject(
    QDialog *dialog,
    const QString &title,
    const QString &message,
    const QString &details = QString(),
    const MessageExecutor &executor = {});

bool showAndAccept(
    QDialog *dialog,
    const QString &title,
    const QString &message,
    const MessageExecutor &executor = {});

bool showDetached(
    const QString &title,
    const QString &message,
    const QString &details = QString(),
    const MessageExecutor &executor = {});

} // namespace OAuthDialogMessageGuard

#endif
