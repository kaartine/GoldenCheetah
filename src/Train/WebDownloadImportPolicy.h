/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_WebDownloadImportPolicy_h
#define GC_WebDownloadImportPolicy_h

#include <QHash>
#include <QString>
#include <QUrl>
#include <QtGlobal>

namespace WebDownloadImportPolicy {

enum class UserDecision {
    NotAsked,
    Rejected,
    Approved
};

enum class RequestAction {
    Ignore,
    Cancel,
    Confirm,
    Accept
};

struct Request {
    quint32 id = 0;
    quintptr sourcePage = 0;
    quintptr expectedPage = 0;
    bool pageVisible = false;
    bool savePageDownload = false;
    qint64 totalBytes = -1;
    QUrl pageUrl;
    QUrl downloadUrl;
    QString suggestedFileName;
};

struct Decision {
    RequestAction action = RequestAction::Cancel;
    QString fileName;
    QString filePath;
};

struct Completion {
    quint32 id = 0;
    quintptr sourcePage = 0;
    QString filePath;
    bool finished = false;
    bool succeeded = false;
};

class Gate
{
public:
    Decision handleRequest(const Request &request,
                           const QString &stagingDirectory,
                           UserDecision userDecision);
    QString takeCompletedImport(const Completion &completion);
    void cancel(quint32 id);

    bool hasPending(quint32 id) const;
    int awaitingCount() const;
    int pendingCount() const;

    static QString safeFileName(const QString &suggestedFileName);
    static qint64 maximumImportBytes();

private:
    struct Pending {
        quintptr sourcePage = 0;
        QString stagingDirectory;
        QString filePath;
    };

    static bool isValidRequest(const Request &request);
    static bool isSameRequest(const Request &left,
                              const Request &right);
    static bool prepareDestination(const QString &stagingDirectory,
                                   const QString &fileName,
                                   Pending *pending);

    QHash<quint32, Request> awaiting_;
    QHash<quint32, Pending> pending_;
};

} // namespace WebDownloadImportPolicy

#endif
