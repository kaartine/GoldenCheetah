/*
 * Copyright (c) 2015 Joern Rischmueller (joern.rm@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
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

#include "LocalFileStore.h"

#include "LocalFileStoreProcess.h"
#include "Settings.h"

#include <QDir>
#include <QFileInfo>

#include <limits>
#include <utility>

namespace {

unsigned long entrySize(qint64 size)
{
    if (size <= 0) return 0;
    const quint64 unsignedSize = quint64(size);
    const quint64 maximum =
            quint64(std::numeric_limits<unsigned long>::max());
    return unsignedSize > maximum
        ? std::numeric_limits<unsigned long>::max()
        : static_cast<unsigned long>(unsignedSize);
}

#ifdef Q_OS_UNIX
class LocalFileStoreAutoDownload final : public CloudService
{
public:
    explicit LocalFileStoreAutoDownload(Context *context)
        : CloudService(context)
    {
        settings.insert(Folder, GC_NETWORKFILESTORE_FOLDER);
    }

    CloudService *clone(Context *context) override
    {
        return new LocalFileStoreAutoDownload(context);
    }

    AutoDownloadExecution autoDownloadExecution() const override
    {
        return AutoDownloadExecution::ProcessIsolated;
    }

    QString id() const override { return QStringLiteral("Local Store"); }
    QString uiName() const override { return tr("Local Store"); }
    QString description() const override
    {
        return tr("Sync with a local folder or thumbdrive.");
    }
    QImage logo() const override
    {
        return QImage(QStringLiteral(":images/services/localstore.png"));
    }

    bool open(QStringList &errors) override
    {
        const LocalFileStoreProcessResult result =
                LocalFileStoreProcess::run(
                    LocalFileStoreProcess::Operation::Open,
                    configuredRoot());
        if (!result.succeeded()) {
            errors << tr("The local folder is not accessible");
            return false;
        }
        return true;
    }

    bool close() override { return true; }
    QString home() override { return QStringLiteral("/"); }

    QList<CloudServiceEntry *> readdir(
            QString path, QStringList &errors) override
    {
        const LocalFileStoreProcessResult result =
                LocalFileStoreProcess::run(
                    LocalFileStoreProcess::Operation::List,
                    configuredRoot(), path);
        if (!result.succeeded()) {
            errors << tr("The local folder could not be listed");
            return {};
        }
        QList<CloudServiceEntry *> entries;
        entries.reserve(result.entries.size());
        for (const LocalFileStoreEntryValue &value : result.entries) {
            CloudServiceEntry *entry = newCloudServiceEntry();
            entry->name = value.name;
            entry->id = value.name;
            entry->isDir = value.isDir;
            entry->size = entrySize(value.size);
            entry->modified = value.modified;
            entries.append(entry);
        }
        return entries;
    }

    bool readFile(
            QByteArray *data,
            QString remoteName,
            QString) override
    {
        if (!data) return false;

        LocalFileStoreProcessResult result =
                LocalFileStoreProcess::run(
                    LocalFileStoreProcess::Operation::Read,
                    configuredRoot(), remoteName);
        if (!result.succeeded()) return false;

        *data = std::move(result.data);
        notifyReadComplete(data, remoteName, tr("Completed."));
        return true;
    }

private:
    QString configuredRoot() const
    {
        return getSetting(GC_NETWORKFILESTORE_FOLDER).toString();
    }
};
#endif

} // namespace

LocalFileStore::LocalFileStore(Context *context)
    : CloudService(context)
{
    if (context) {
        root_ = newCloudServiceEntry();
        root_->name = QStringLiteral("/");
        root_->isDir = true;
        root_->size = 1;
    }

    settings.insert(Folder, GC_NETWORKFILESTORE_FOLDER);
}

bool LocalFileStore::isSupportedPlatform()
{
#ifdef Q_OS_UNIX
    return true;
#else
    return false;
#endif
}

LocalFileStore::~LocalFileStore() = default;

CloudService *LocalFileStore::cloneForAutoDownload(
        Context *context)
{
#ifdef Q_OS_UNIX
    return new LocalFileStoreAutoDownload(context);
#else
    Q_UNUSED(context)
    return nullptr;
#endif
}

bool LocalFileStore::open(QStringList &errors)
{
#ifndef Q_OS_UNIX
    errors << tr("Local Store is not supported on this platform");
    return false;
#else
    const QString folder =
            getSetting(GC_NETWORKFILESTORE_FOLDER).toString();
    if (folder.isEmpty() || !QDir(folder).exists()) {
        errors << tr("The local folder does not exist or is not accessible");
        return false;
    }
    return true;
#endif
}

bool LocalFileStore::close()
{
    return true;
}

QString LocalFileStore::home()
{
#ifndef Q_OS_UNIX
    return {};
#else
    return getSetting(GC_NETWORKFILESTORE_FOLDER).toString();
#endif
}

bool LocalFileStore::createFolder(QString path)
{
#ifndef Q_OS_UNIX
    Q_UNUSED(path)
    return false;
#else
    const QFileInfo target(path);
    QDir parent = target.dir();
    return parent.exists() && parent.mkdir(target.fileName());
#endif
}

QList<CloudServiceEntry *>
LocalFileStore::readdir(QString path, QStringList &errors)
{
#ifndef Q_OS_UNIX
    Q_UNUSED(path)
    errors << tr("Local Store is not supported on this platform");
    return {};
#else
    QDir directory(path);
    if (!directory.exists()) {
        errors << tr("The local folder does not exist");
        return {};
    }

    QList<CloudServiceEntry *> entries;
    const QFileInfoList files = directory.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot);
    entries.reserve(files.size());
    for (const QFileInfo &info : files) {
        CloudServiceEntry *entry = newCloudServiceEntry();
        entry->name = info.fileName();
        entry->id = entry->name;
        entry->isDir = info.isDir();
        entry->size = entrySize(info.size());
        entry->modified = info.lastModified();
        entries.append(entry);
    }
    return entries;
#endif
}

bool LocalFileStore::readFile(
        QByteArray *data, QString remoteName, QString)
{
    if (!data) return false;

#ifndef Q_OS_UNIX
    Q_UNUSED(remoteName)
    return false;
#else
    const QString folder =
            getSetting(GC_NETWORKFILESTORE_FOLDER).toString();
    LocalFileStoreProcessResult result =
            LocalFileStoreProcess::readFileInProcess(
                folder, remoteName);
    if (!result.succeeded()) return false;

    *data = std::move(result.data);
    notifyReadComplete(data, remoteName, tr("Completed."));
    return true;
#endif
}

bool LocalFileStore::writeFile(
        QByteArray &data, QString remoteName, RideFile *ride)
{
    Q_UNUSED(ride)

#ifndef Q_OS_UNIX
    Q_UNUSED(data)
    Q_UNUSED(remoteName)
    return false;
#else
    const QString folder =
            getSetting(GC_NETWORKFILESTORE_FOLDER).toString();
    const LocalFileStoreProcessResult result =
            LocalFileStoreProcess::writeFileInProcess(
                folder, remoteName, data);
    if (!result.succeeded()) {
        notifyWriteComplete(
            QString(), tr("Write to the local folder failed"));
        return false;
    }

    notifyWriteComplete(QString(), tr("Completed."));
    return true;
#endif
}

static bool addLocalFileStore()
{
    if (LocalFileStore::isSupportedPlatform()) {
        CloudServiceFactory::instance().addService(
            new LocalFileStore(nullptr));
    }
    return true;
}

static bool add = addLocalFileStore();
