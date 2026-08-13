/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "OAuthDialogMessageGuard.h"

#include <QDialog>
#include <QPointer>

namespace OAuthDialogMessageGuard {

namespace {

enum class DialogCompletion
{
    Accept,
    Reject
};

bool showGuarded(
    QDialog *dialog,
    QMessageBox::Icon icon,
    const QString &title,
    const QString &message,
    const QString &details,
    DialogCompletion completion,
    const MessageExecutor &executor)
{
    if (!dialog) return false;

    const QPointer<QDialog> guardedDialog(dialog);
    QPointer<QMessageBox> guardedMessage = new QMessageBox(
        icon,
        title,
        message,
        QMessageBox::NoButton,
        dialog);
    guardedMessage->setAttribute(Qt::WA_DeleteOnClose);
    guardedMessage->setWindowModality(Qt::ApplicationModal);
    if (!details.isEmpty())
        guardedMessage->setDetailedText(details);
    QObject::connect(
        guardedMessage, &QDialog::finished,
        guardedMessage,
        [guardedDialog, completion] {
            if (!guardedDialog) return;
            if (completion == DialogCompletion::Accept)
                guardedDialog->accept();
            else
                guardedDialog->reject();
        });
    if (executor) executor(*guardedMessage);
    else guardedMessage->open();
    return guardedDialog && guardedMessage;
}

} // namespace

bool showAndReject(
    QDialog *dialog,
    const QString &title,
    const QString &message,
    const QString &details,
    const MessageExecutor &executor)
{
    return showGuarded(
        dialog,
        QMessageBox::Critical,
        title,
        message,
        details,
        DialogCompletion::Reject,
        executor);
}

bool showAndAccept(
    QDialog *dialog,
    const QString &title,
    const QString &message,
    const MessageExecutor &executor)
{
    return showGuarded(
        dialog,
        QMessageBox::Information,
        title,
        message,
        QString(),
        DialogCompletion::Accept,
        executor);
}

bool showDetached(
    const QString &title,
    const QString &message,
    const QString &details,
    const MessageExecutor &executor)
{
    QPointer<QMessageBox> guardedMessage = new QMessageBox(
        QMessageBox::Critical,
        title,
        message);
    guardedMessage->setAttribute(Qt::WA_DeleteOnClose);
    guardedMessage->setWindowModality(Qt::ApplicationModal);
    if (!details.isEmpty())
        guardedMessage->setDetailedText(details);
    if (executor) executor(*guardedMessage);
    else guardedMessage->open();
    return guardedMessage;
}

} // namespace OAuthDialogMessageGuard
