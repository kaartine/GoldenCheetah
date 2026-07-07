/*
 * Copyright (c) 2015 Joern Rischmueller (joern.rm@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "AthleteBackup.h"

#include "AthleteBackupArchive.h"
#include "GcUpgrade.h"
#include "Settings.h"

#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QProgressDialog>
#include <QStorageInfo>

#include <limits>
#include <utility>

AthleteBackup::AthleteBackup(QDir athleteHome)
    : athleteHome(std::move(athleteHome)),
      globalHome(this->athleteHome),
      athlete(this->athleteHome.dirName())
{
    globalHome.cdUp();
}

AthleteBackup::~AthleteBackup() = default;

void AthleteBackup::backupOnClose()
{
    const int backupPeriod =
        appsettings->cvalue(athlete, GC_AUTOBACKUP_PERIOD, 0).toInt();
    backupFolder =
        appsettings->cvalue(athlete, GC_AUTOBACKUP_FOLDER, "").toString();
    if (backupPeriod == 0 || backupFolder.isEmpty()) return;

    int backupCounter =
        appsettings->cvalue(athlete, GC_AUTOBACKUP_COUNTER, 0).toInt();
    ++backupCounter;
    if (backupCounter < backupPeriod) {
        appsettings->setCValue(
            athlete, GC_AUTOBACKUP_COUNTER, backupCounter);
        return;
    }

    backup(tr("Abort Backup and Reset Counter"));
    appsettings->setCValue(athlete, GC_AUTOBACKUP_COUNTER, 0);
}

void AthleteBackup::backupImmediate()
{
    backupFolder =
        appsettings->cvalue(athlete, GC_AUTOBACKUP_FOLDER, "").toString();
    const QString directory = QFileDialog::getExistingDirectory(
        nullptr,
        tr("Select Backup Directory"),
        backupFolder,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (directory.isEmpty()) {
        QMessageBox::information(
            nullptr,
            tr("Athlete Backup"),
            tr("No backup directory selected - backup aborted"));
        return;
    }

    backupFolder = directory;
    QMessageBox confirmation;
    confirmation.setWindowTitle(tr("Athlete Backup"));
    confirmation.setText(
        tr("Any unsaved data will not be included into the backup .zip file."));
    confirmation.setInformativeText(tr("Do you want to proceed?"));
    confirmation.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    confirmation.setDefaultButton(QMessageBox::Yes);
    if (confirmation.exec() == QMessageBox::No) return;

    if (backup(tr("Abort Backup"))) {
        QMessageBox::information(
            nullptr,
            tr("Athlete Backup"),
            tr("Backup successfully stored in \n%1").arg(backupFolder));
    }
}

bool AthleteBackup::backup(const QString &progressText)
{
    AthleteBackupManifest manifest;
    qint64 fileSize = 0;
    QString error;
    if (!buildAthleteBackupManifest(
            athleteHome,
            globalHome,
            manifest,
            fileSize,
            error)) {
        QMessageBox::warning(
            nullptr,
            tr("Athlete Backup"),
            tr("The backup could not be created: %1").arg(error));
        return false;
    }

    if (manifest.isEmpty()) {
        QMessageBox::information(
            nullptr,
            tr("Athlete Backup"),
            tr("No files found for athlete %1 - all athlete sub-directories are empty.")
                .arg(athlete));
        return false;
    }

    const QStorageInfo storage(backupFolder);
    if (!storage.isValid() || !storage.isReady()) {
        QMessageBox::warning(
            nullptr,
            tr("Athlete Backup"),
            tr("Directory %1 not available. No backup .zip file created for athlete %2.")
                .arg(backupFolder, athlete));
        return false;
    }

    const qint64 archiveOverhead = qMax<qint64>(
        1024 * 1024,
        static_cast<qint64>(manifest.size()) * 512);
    if (fileSize > std::numeric_limits<qint64>::max() - archiveOverhead
        || storage.bytesAvailable() < fileSize + archiveOverhead) {
        QMessageBox::warning(
            nullptr,
            tr("Athlete Backup"),
            tr("Not enough space available on disk: %1 - no backup .zip file created")
                .arg(storage.rootPath()));
        return false;
    }

    const QDateTime now = QDateTime::currentDateTime();
    const QString targetFileName =
        QStringLiteral("GC_%1_%2_%3.zip")
            .arg(
                QString::number(VERSION_LATEST),
                athlete,
                now.toString(QStringLiteral("yyyy_MM_dd_HH_mm_ss")));
    const QString targetPath =
        QDir(backupFolder).filePath(targetFileName);

    QProgressDialog progress(
        tr("Adding files to backup %1 for athlete %2 ...")
            .arg(targetFileName, athlete),
        progressText,
        0,
        manifest.size(),
        nullptr);
    progress.setWindowModality(Qt::WindowModal);

    const AthleteBackupProgressFunction updateProgress =
        [&progress](int completed, int total) {
            progress.setMaximum(total);
            progress.setValue(completed);
            return !progress.wasCanceled();
        };

    if (!publishVerifiedAthleteBackup(
            targetPath,
            manifest,
            error,
            athleteBackupFileSourceFactory(),
            updateProgress)) {
        if (error != QStringLiteral("Backup canceled")) {
            QMessageBox::warning(
                nullptr,
                tr("Athlete Backup"),
                tr("The backup could not be created: %1").arg(error));
        }
        return false;
    }

    progress.setValue(manifest.size());
    return true;
}
