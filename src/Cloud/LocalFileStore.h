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

#ifndef GC_LocalFileStore_h
#define GC_LocalFileStore_h

#include "CloudService.h"
#include <QImage>

class LocalFileStore : public CloudService {

    Q_OBJECT

    public:

        LocalFileStore(Context *context);
        static bool isSupportedPlatform();
        CloudService *clone(Context *context) { return new LocalFileStore(context); }
        AutoDownloadExecution autoDownloadExecution() const override
        {
#ifdef Q_OS_UNIX
            return AutoDownloadExecution::ProcessIsolated;
#else
            return AutoDownloadExecution::Unsupported;
#endif
        }
        CloudService *cloneForAutoDownload(
            Context *context) override;

        int capabilities() const override {
            return isSupportedPlatform()
                ? Upload | Download | Query
                : 0;
        }

        ~LocalFileStore() override;

        QString id() const override { return "Local Store"; }
        QString uiName() const override { return tr("Local Store"); }
        QString description() const override
            { return tr("Sync with a local folder or thumbdrive."); }
        QImage logo() const override
            { return QImage(":images/services/localstore.png"); }


        // open/connect and close/disconnect
        bool open(QStringList &errors) override;
        bool close() override;

        // home directory
        QString home() override;

        // write a file 
        bool writeFile(
            QByteArray &data, QString remotename, RideFile *ride) override;

        // read a file
        bool readFile(
            QByteArray *data, QString remotename, QString) override;

        // create a folder
        bool createFolder(QString path) override;

        // dirent style api
        CloudServiceEntry *root() override { return root_; }
        QList<CloudServiceEntry *> readdir(
            QString path, QStringList &errors) override;

    private:
        CloudServiceEntry *root_ = nullptr;

};
#endif
