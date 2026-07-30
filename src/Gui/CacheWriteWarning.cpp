/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "CacheWriteWarning.h"

#include <QCoreApplication>
#include <QMessageBox>

QMessageBox *
CacheWriteWarning::show(
    QWidget *owner,
    const QString &message)
{
    auto *warning = new QMessageBox(
        QMessageBox::Warning,
        QCoreApplication::translate(
            "MainWindow",
            "Cache Write Failed"),
        message,
        QMessageBox::Ok,
        owner);
    warning->setAttribute(Qt::WA_DeleteOnClose);
    warning->setModal(false);
    warning->setWindowModality(Qt::NonModal);
    warning->show();
    return warning;
}
