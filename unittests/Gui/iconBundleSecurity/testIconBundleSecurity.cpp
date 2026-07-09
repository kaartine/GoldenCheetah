/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Gui/IconManager.h"
#include "zipwriter.h"

#include <QFile>
#include <QHostAddress>
#include <QMap>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <cstddef>
#include <cstring>
#include <utility>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#include <winioctl.h>
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
#endif

QString gcroot;

QString RideFile::sportTag(QString sport)
{
    return sport;
}

QString RideItem::getText(QString, QString fallback) const
{
    return fallback;
}

class TlsHttpServer : public QTcpServer
{
public:
    TlsHttpServer(const QSslCertificate &certificate,
                  const QSslKey &key,
                  QByteArray response,
                  QObject *parent = nullptr)
        : QTcpServer(parent)
        , certificate(certificate)
        , key(key)
        , response(std::move(response))
    {
    }

    int connectionCount = 0;

protected:
    void incomingConnection(qintptr socketDescriptor) override
    {
        auto *socket = new QSslSocket(this);
        socket->setLocalCertificate(certificate);
        socket->setPrivateKey(key);
        if (!socket->setSocketDescriptor(socketDescriptor)) {
            socket->deleteLater();
            return;
        }

        ++connectionCount;
        connect(socket, &QSslSocket::encrypted, socket, [this, socket]() {
            connect(socket, &QSslSocket::readyRead, socket, [this, socket]() {
                if (socket->property("responded").toBool())
                    return;
                socket->readAll();
                socket->setProperty("responded", true);
                socket->write(response);
                socket->disconnectFromHost();
            });
        });
        connect(socket, &QSslSocket::errorOccurred, socket,
                [socket]() { socket->disconnectFromHost(); });
        connect(socket, &QSslSocket::disconnected,
                socket, &QObject::deleteLater);
        socket->startServerEncryption();
    }

private:
    QSslCertificate certificate;
    QSslKey key;
    QByteArray response;
};

class DefaultSslConfigurationGuard
{
public:
    DefaultSslConfigurationGuard()
        : original(QSslConfiguration::defaultConfiguration())
    {
    }

    ~DefaultSslConfigurationGuard()
    {
        QSslConfiguration::setDefaultConfiguration(original);
    }

private:
    QSslConfiguration original;
};

class TestIconBundleSecurity : public QObject
{
    Q_OBJECT

    using BundleEntries = QList<QPair<QString, QByteArray>>;
    using IconState = QMap<QString, QByteArray>;

    QTemporaryDir root;
    IconManager *iconManager = nullptr;

    QString profileDirectory() const
    {
        return root.filePath("profile/current");
    }

    QString iconDirectory() const
    {
        return QDir(profileDirectory()).filePath(".icons");
    }

    QString iconPath(const QString &name) const
    {
        return QDir(iconDirectory()).filePath(name);
    }

    static QByteArray svgData(QByteArray color = "#234f7d")
    {
        return QByteArray(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" "
            "height=\"8\"><path fill=\"")
            + color + "\" d=\"M0 0h8v8H0z\"/></svg>";
    }

    static QByteArray mappingData(QByteArray filename = "bike.svg")
    {
        return QByteArray("{\"Sport\":{\"Bike\":\"") + filename
            + "\"},\"SubSport\":{}}";
    }

    // Public localhost test fixture; it protects no real service or credential.
    static QByteArray tlsCertificatePem()
    {
        return QByteArray(R"PEM(-----BEGIN CERTIFICATE-----
MIIDJTCCAg2gAwIBAgIUd5a90uoa7mAiFMCBM/HqhFkUcYUwDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDcwNTE5MjIyNloXDTM2MDcw
MjE5MjIyNlowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEA+BERHE0gvpXLe/b/Nq3v6Qx2vWhk3lpaSmxf0frJYno2
zUEVn0pxOMFt4PlSvAvx3drJarhscwved5NwbBdEJY8ZItAetp2NmdC7I35EqWgr
dofPaDOylp7vRrDbGcVWqhBypBhXD03QXcaHmNmh5SXPv90GXJcDLFrEbiHzDRLL
83LX6dvagbLv6a37dPayk9XOhXfnAjDSMoCLbpCwG1jj0xpenwKmo4Aa4dghtsy4
7jrQAh6aWXAyBCzemgNYRkILOUiT2Q3246ax6IqlkpPsh5iAN2Oj/EyBHD7hxW8r
jPwyxakQfCJRExhkqiFwHXEFcSYdT8KYT69NePKnlwIDAQABo28wbTAdBgNVHQ4E
FgQU8z5GNBOnC4cDJt4ZnAgc/yD/O9EwHwYDVR0jBBgwFoAU8z5GNBOnC4cDJt4Z
nAgc/yD/O9EwDwYDVR0TAQH/BAUwAwEB/zAaBgNVHREEEzARgglsb2NhbGhvc3SH
BH8AAAEwDQYJKoZIhvcNAQELBQADggEBAMBi4qlLM0CD57oI8yWNiSor0Heqb/Gm
kwgdRtR5/YutFg4ZO8++gP8z5MGL6tLFQAY3VTC5PQ5s3cArCxczNn/8KDiTjknI
iXk7wp/20xRwRphyRVpN9gxN8qxiE3kdPw8dh+LNKYtlEVfCKZdjEpyPxT5iaCRI
b2XQEC3K2Bv+8s8LBhSQTxi+tyfKYUAFWzAG5g0zqtZrkehvyXA7fG8j1kKoVrrK
Tx6SJzSWmgP0m3dl7XdbmQvkY388dY2ntqTQgpYsf6NbLeSM8HkdovNV1l7F5Vog
U3+sZht85KCIYISAKFHSZJHDeeN/EaEID1xE56jwZIR5JtuQy2kBBXI=
-----END CERTIFICATE-----
)PEM");
    }

    static QByteArray tlsPrivateKeyPem()
    {
        return QByteArray(R"PEM(-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQD4EREcTSC+lct7
9v82re/pDHa9aGTeWlpKbF/R+sliejbNQRWfSnE4wW3g+VK8C/Hd2slquGxzC953
k3BsF0Qljxki0B62nY2Z0LsjfkSpaCt2h89oM7KWnu9GsNsZxVaqEHKkGFcPTdBd
xoeY2aHlJc+/3QZclwMsWsRuIfMNEsvzctfp29qBsu/prft09rKT1c6Fd+cCMNIy
gItukLAbWOPTGl6fAqajgBrh2CG2zLjuOtACHppZcDIELN6aA1hGQgs5SJPZDfbj
prHoiqWSk+yHmIA3Y6P8TIEcPuHFbyuM/DLFqRB8IlETGGSqIXAdcQVxJh1PwphP
r0148qeXAgMBAAECggEAb7SDv3pNxfS/SkXJ3A4MTCXkLFufnG8UXJIbvfsQB2vg
VKI7wdysSnHz8ExlgK9iTVRxwcBleZL9LVxz0gDSG5WqLH6KbhJZisuvEYcRFTYG
6Fu0vogVIfVU60DFkP/HjFJWxWT+gzI3o5q8QcyytfTGbe/yJuyLrcP701ovUBFl
VW2ug1ZcaUu0h7oINYUoXdVUk5b3xswJe3LZkl5jrJZFaP1mA2oaUhfvY/G/BtGe
KpbhgGKQS2n587jafoCqv4Fn9hKD6G4k73kfp4TOgghqODSD01Uyr9G7/1H5e3yH
9vLfUH3TOFbdnvzSJ5o4Efs0emZNk8B1UFIrYiTwIQKBgQD+Ol10nEnSpi4aJ3NH
s0HjLRd2tuiTt4O+041AV15pA8k+UNqCxPLeONzG/pUNDK+b2lo4WU2IJgDhmNqp
LZ13UVtwftg83O90VOZsUf7qzXqOQqvYejqyHDp67ycdFdQstvMww8HoIToWnuX4
+NsrO3zh/ll4k2t8lZ8fRQw0NwKBgQD5y7Uu1I1p5XNFPZ/uSJw5GM4VdKbY/9wB
0ExcBGUCuTeBDBg3Rl3+x0n8//O4tp3sMLK9+0AtjnEenv0Xf1MTC+nDInpH8Y6z
FikrCugzTf5OQUHkNNfNdqupScjGYLoyx5fqPYE72HDOQLNm3r00bfah/O90bkk6
eLESEGI3oQKBgQCtH2fG4isvhLT+YIETgZHLt0g0MoidFypjR8L33sdO8iIYCo1S
4fWVuNk3teQgd9QaaQ5pMv8mSOLuvd2huYty1ndTWz277KQv7yTe/NOAaB8eQ26s
w0e3RJvaXYOgPd43+PoQ6i3g+seI4fovmp/9h8waRc/92T4oH+e1LpsJ7wKBgGz4
rwvE8gQWctjr708WIgDOj2jQwNC9nY60/frOd/peLpZ/XjkO24aADgju57XXBMG6
5N74MqEtGLARD7wmcWlHsyzLZ6jm+ieqLT93vKBVD/G3ijUx0Erz0CND6vCP1eq9
I8quFiizXVK86wafeM7cgdFc9GFIk22MpUKKHy7BAoGAOZ5vMG/Gxh/CW7acbCxO
G03/kQI08MaZXpP1xnXuY5LoRRtmqbOkWSrVVuK8UJv7h1ukl8NMQUBWZmf3TrSx
nXlLywLt9sk+p3QGJDMzB1d4IKqb8Ccx5ESmspupvyOcPMQV1NqyJ9pwbeylZ4OM
cNHzIdlUHCfnKWbwCR6co0w=
-----END PRIVATE KEY-----
)PEM");
    }

    static QByteArray httpResponse(const QByteArray &statusAndHeaders,
                                   const QByteArray &body = {})
    {
        return statusAndHeaders
            + "\r\nContent-Length: " + QByteArray::number(body.size())
            + "\r\nConnection: close\r\n\r\n" + body;
    }

    QString createBundle(const QString &name, const BundleEntries &entries) const
    {
        const QString archivePath = root.filePath(name);
        ZipWriter writer(archivePath);
        if (!writer.isWritable())
            return QString();

        for (const auto &entry : entries)
            writer.addFile(entry.first, entry.second);
        writer.close();

        return writer.status() == ZipWriter::NoError
            ? archivePath
            : QString();
    }

    static QByteArray readFile(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return QByteArray();
        return file.readAll();
    }

    static bool writeFile(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        return file.open(QIODevice::WriteOnly)
            && file.write(contents) == contents.size();
    }

    IconState captureIconState() const
    {
        IconState state;
        const QFileInfoList entries = QDir(iconDirectory()).entryInfoList(
            QDir::AllEntries | QDir::Hidden | QDir::System
                | QDir::NoDotAndDotDot,
            QDir::Name);
        for (const QFileInfo &entry : entries) {
            if (entry.isSymLink()) {
                state.insert(entry.fileName(),
                             "link:" + entry.symLinkTarget().toUtf8());
            } else if (entry.isDir()) {
                state.insert(entry.fileName(), "directory");
            } else {
                state.insert(entry.fileName(), readFile(entry.absoluteFilePath()));
            }
        }
        return state;
    }

    void verifyUnchanged(const IconState &before,
                         const QString &assignedIcon) const
    {
        QCOMPARE(captureIconState(), before);
        QCOMPARE(iconManager->assignedIcon("Sport", "Bike"), assignedIcon);
    }

    static bool createDirectoryLink(const QString &target,
                                    const QString &link)
    {
#if defined(Q_OS_WIN)
        const QString nativeTarget = QDir::toNativeSeparators(target);
        const QString nativeLink = QDir::toNativeSeparators(link);
        DWORD flags = SYMBOLIC_LINK_FLAG_DIRECTORY
            | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
        if (CreateSymbolicLinkW(
                reinterpret_cast<LPCWSTR>(nativeLink.utf16()),
                reinterpret_cast<LPCWSTR>(nativeTarget.utf16()),
                flags)) {
            return true;
        }
        if (GetLastError() == ERROR_INVALID_PARAMETER) {
            flags = SYMBOLIC_LINK_FLAG_DIRECTORY;
            return CreateSymbolicLinkW(
                reinterpret_cast<LPCWSTR>(nativeLink.utf16()),
                reinterpret_cast<LPCWSTR>(nativeTarget.utf16()),
                flags);
        }
        return false;
#else
        return QFile::link(target, link);
#endif
    }

#if defined(Q_OS_WIN)
    static bool createDirectoryJunction(const QString &target,
                                        const QString &link)
    {
        struct JunctionReparseData {
            DWORD tag;
            WORD dataLength;
            WORD reserved;
            WORD substituteNameOffset;
            WORD substituteNameLength;
            WORD printNameOffset;
            WORD printNameLength;
            WCHAR pathBuffer[1];
        };

        const QString printName = QDir::toNativeSeparators(
            QFileInfo(target).absoluteFilePath());
        const QString substituteName = printName.startsWith("\\\\")
            ? QStringLiteral("\\??\\UNC\\") + printName.mid(2)
            : QStringLiteral("\\??\\") + printName;
        const qsizetype substituteBytes =
            substituteName.size() * sizeof(WCHAR);
        const qsizetype printBytes = printName.size() * sizeof(WCHAR);
        const qsizetype pathBytes = substituteBytes + sizeof(WCHAR)
            + printBytes + sizeof(WCHAR);
        const qsizetype totalBytes =
            offsetof(JunctionReparseData, pathBuffer) + pathBytes;
        if (totalBytes > MAXIMUM_REPARSE_DATA_BUFFER_SIZE)
            return false;

        const QString nativeLink = QDir::toNativeSeparators(link);
        if (!CreateDirectoryW(
                reinterpret_cast<LPCWSTR>(nativeLink.utf16()), nullptr)) {
            return false;
        }

        QByteArray storage(totalBytes, '\0');
        auto *data =
            reinterpret_cast<JunctionReparseData *>(storage.data());
        data->tag = IO_REPARSE_TAG_MOUNT_POINT;
        data->dataLength = static_cast<WORD>(totalBytes - 8);
        data->substituteNameLength =
            static_cast<WORD>(substituteBytes);
        data->printNameOffset = static_cast<WORD>(
            substituteBytes + sizeof(WCHAR));
        data->printNameLength = static_cast<WORD>(printBytes);

        char *pathBuffer = reinterpret_cast<char *>(data->pathBuffer);
        std::memcpy(pathBuffer, substituteName.utf16(), substituteBytes);
        std::memcpy(pathBuffer + data->printNameOffset,
                    printName.utf16(), printBytes);

        const HANDLE handle = CreateFileW(
            reinterpret_cast<LPCWSTR>(nativeLink.utf16()),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            RemoveDirectoryW(
                reinterpret_cast<LPCWSTR>(nativeLink.utf16()));
            return false;
        }

        DWORD returned = 0;
        const bool created = DeviceIoControl(
            handle,
            FSCTL_SET_REPARSE_POINT,
            data,
            static_cast<DWORD>(totalBytes),
            nullptr,
            0,
            &returned,
            nullptr);
        CloseHandle(handle);
        if (!created) {
            RemoveDirectoryW(
                reinterpret_cast<LPCWSTR>(nativeLink.utf16()));
        }
        return created;
    }

    static DWORD reparseTag(const QString &path)
    {
        WIN32_FIND_DATAW data = {};
        const QString nativePath = QDir::toNativeSeparators(path);
        const HANDLE handle = FindFirstFileW(
            reinterpret_cast<LPCWSTR>(nativePath.utf16()), &data);
        if (handle == INVALID_HANDLE_VALUE)
            return 0;
        FindClose(handle);
        return data.dwReserved0;
    }
#endif

    static bool createDirectoryRedirection(const QString &target,
                                           const QString &link)
    {
#if defined(Q_OS_WIN)
        return createDirectoryJunction(target, link);
#else
        return createDirectoryLink(target, link);
#endif
    }

    static bool createFileLink(const QString &target, const QString &link)
    {
#if defined(Q_OS_WIN)
        const QString nativeTarget = QDir::toNativeSeparators(target);
        const QString nativeLink = QDir::toNativeSeparators(link);
        DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
        if (CreateSymbolicLinkW(
                reinterpret_cast<LPCWSTR>(nativeLink.utf16()),
                reinterpret_cast<LPCWSTR>(nativeTarget.utf16()),
                flags)) {
            return true;
        }
        if (GetLastError() == ERROR_INVALID_PARAMETER) {
            return CreateSymbolicLinkW(
                reinterpret_cast<LPCWSTR>(nativeLink.utf16()),
                reinterpret_cast<LPCWSTR>(nativeTarget.utf16()),
                0);
        }
        return false;
#else
        return QFile::link(target, link);
#endif
    }

    static bool removeDirectoryLink(const QString &link)
    {
#if defined(Q_OS_WIN)
        const QString nativeLink = QDir::toNativeSeparators(link);
        return RemoveDirectoryW(
            reinterpret_cast<LPCWSTR>(nativeLink.utf16()));
#else
        return QFile::remove(link);
#endif
    }

    static bool removeFileLink(const QString &link)
    {
        return QFile::remove(link);
    }

private slots:

    void initTestCase()
    {
        QVERIFY(root.isValid());
        gcroot = profileDirectory();
        QVERIFY(QDir().mkpath(iconDirectory()));
        QVERIFY(writeFile(iconPath("mapping.json"), "{}"));
        iconManager = &IconManager::instance();
    }

    void init()
    {
        IconManager::setBundleCommitHookForTest({});
        IconManager::setBundleValidationHookForTest({});
        QVERIFY(QDir(root.filePath("profile")).removeRecursively());
        QVERIFY(QDir().mkpath(iconDirectory()));
        const QString baseline = createBundle(
            "baseline.zip",
            {
                { "old.svg", svgData("#767676") },
                { "README.txt", "existing license" },
                { "mapping.json", mappingData("old.svg") },
            });
        QVERIFY(!baseline.isEmpty());
        QVERIFY(iconManager->importBundle(baseline));
    }

    void cleanup()
    {
        IconManager::setBundleCommitHookForTest({});
        IconManager::setBundleValidationHookForTest({});
    }

    void rejectsTraversalBeforeWriting()
    {
        const IconState before = captureIconState();
        const QString assigned = iconManager->assignedIcon("Sport", "Bike");

        const QString victim = root.filePath("traversal-victim.svg");
        QVERIFY(writeFile(victim, "original"));
        const QString archive = createBundle(
            "traversal.zip",
            {
                { "bike.svg", svgData() },
                { "../traversal-victim.svg", "overwritten" },
                { "mapping.json", mappingData() },
            });
        QVERIFY(!archive.isEmpty());

        const bool imported = iconManager->importBundle(archive);

        QCOMPARE(readFile(victim), QByteArray("original"));
        QVERIFY(!imported);
        verifyUnchanged(before, assigned);
    }

    void rejectsAbsolutePathBeforeWriting()
    {
        const IconState before = captureIconState();
        const QString assigned = iconManager->assignedIcon("Sport", "Bike");

        const QString victim = root.filePath("absolute-victim.svg");
        QVERIFY(writeFile(victim, "original"));
        const QString archive = createBundle(
            "absolute.zip",
            {
                { "bike.svg", svgData() },
                { victim, "overwritten" },
                { "mapping.json", mappingData() },
            });
        QVERIFY(!archive.isEmpty());

        const bool imported = iconManager->importBundle(archive);

        QCOMPARE(readFile(victim), QByteArray("original"));
        QVERIFY(!imported);
        verifyUnchanged(before, assigned);
    }

    void rejectsUnexpectedMemberBeforeWriting()
    {
        const IconState before = captureIconState();
        const QString assigned = iconManager->assignedIcon("Sport", "Bike");
        const QString archive = createBundle(
            "unexpected.zip",
            {
                { "bike.svg", svgData() },
                { "payload.bin", "unexpected" },
                { "mapping.json", mappingData() },
            });
        QVERIFY(!archive.isEmpty());

        const bool imported = iconManager->importBundle(archive);

        QVERIFY(!QFileInfo::exists(iconPath("bike.svg")));
        QVERIFY(!QFileInfo::exists(iconPath("payload.bin")));
        QVERIFY(!imported);
        verifyUnchanged(before, assigned);
    }

    void rejectsArchiveLinkBeforeWriting()
    {
        const IconState before = captureIconState();
        const QString assigned = iconManager->assignedIcon("Sport", "Bike");
        const QString archive = root.filePath("archive-link.zip");

        ZipWriter writer(archive);
        QVERIFY(writer.isWritable());
        writer.addFile("bike.svg", svgData());
        writer.addSymLink("linked.svg", "../outside.svg");
        writer.addFile("mapping.json", mappingData());
        writer.close();
        QCOMPARE(writer.status(), ZipWriter::NoError);

        QVERIFY(!iconManager->importBundle(archive));
        verifyUnchanged(before, assigned);
    }

    void rejectsArchiveAliasesAndDuplicates_data()
    {
        QTest::addColumn<QString>("kind");

        QTest::newRow("exact-duplicate") << QString("exact");
        QTest::newRow("case-folded-alias") << QString("case");
        QTest::newRow("unicode-normalization-alias") << QString("unicode");
    }

    void rejectsArchiveAliasesAndDuplicates()
    {
        QFETCH(QString, kind);
        IconState before = captureIconState();
        QString assigned = iconManager->assignedIcon("Sport", "Bike");

        BundleEntries entries;
        QByteArray mappedFilename = "bike.svg";
        entries.append({ "bike.svg", svgData() });
        if (kind == "exact") {
            entries.append({ "bike.svg", svgData("#a23b3b") });
        } else if (kind == "case") {
            entries.append({ "BIKE.svg", svgData("#a23b3b") });
        } else {
            entries.clear();
            const QString composed = QStringLiteral("caf\u00e9.svg");
            entries.append({ composed, svgData() });
            entries.append({ QStringLiteral("cafe\u0301.svg"),
                             svgData("#a23b3b") });
            mappedFilename = composed.toUtf8();

            const QString control = createBundle(
                "unicode-control.zip",
                {
                    { composed, svgData() },
                    { "mapping.json", mappingData(mappedFilename) },
                });
            QVERIFY(!control.isEmpty());
            QVERIFY(iconManager->importBundle(control));
            before = captureIconState();
            assigned = iconManager->assignedIcon("Sport", "Bike");
        }
        entries.append({ "mapping.json", mappingData(mappedFilename) });

        const QString archive = createBundle("alias.zip", entries);
        QVERIFY(!archive.isEmpty());
        QVERIFY(!iconManager->importBundle(archive));
        verifyUnchanged(before, assigned);
    }

    void rejectsCorruptMemberBeforeWriting()
    {
        const IconState before = captureIconState();
        const QString assigned = iconManager->assignedIcon("Sport", "Bike");
        const QString archive = root.filePath("corrupt.zip");

        ZipWriter writer(archive);
        writer.setCompressionPolicy(ZipWriter::NeverCompress);
        writer.addFile("bike.svg", svgData());
        writer.addFile("README.txt", "corrupt-this-payload");
        writer.addFile("mapping.json", mappingData());
        writer.close();

        QFile file(archive);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QByteArray bytes = file.readAll();
        const qsizetype payload = bytes.indexOf("corrupt-this-payload");
        QVERIFY(payload >= 0);
        bytes[payload] = 'C';
        QVERIFY(file.seek(0));
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();

        QVERIFY(!iconManager->importBundle(archive));
        verifyUnchanged(before, assigned);
    }

    void rejectsInvalidMappingWithoutStateChange_data()
    {
        QTest::addColumn<QByteArray>("mapping");

        QTest::newRow("malformed-json") << QByteArray("{");
        QTest::newRow("non-object-root") << QByteArray("[]");
        QTest::newRow("unknown-group")
            << QByteArray("{\"Other\":{}}");
        QTest::newRow("non-object-group")
            << QByteArray("{\"Sport\":[]}");
        QTest::newRow("non-string-assignment")
            << QByteArray("{\"Sport\":{\"Bike\":7}}");
        QTest::newRow("missing-icon")
            << QByteArray("{\"Sport\":{\"Bike\":\"missing.svg\"}}");
        QTest::newRow("metadata-as-icon")
            << QByteArray("{\"Sport\":{\"Bike\":\"README.txt\"}}");
        QTest::newRow("case-mismatched-icon")
            << QByteArray("{\"Sport\":{\"Bike\":\"BIKE.svg\"}}");
    }

    void rejectsInvalidMappingWithoutStateChange()
    {
        QFETCH(QByteArray, mapping);
        const IconState before = captureIconState();
        const QString assigned = iconManager->assignedIcon("Sport", "Bike");
        const QString archive = createBundle(
            "invalid-mapping.zip",
            {
                { "bike.svg", svgData() },
                { "README.txt", "new license" },
                { "mapping.json", mapping },
            });
        QVERIFY(!archive.isEmpty());

        QVERIFY(!iconManager->importBundle(archive));
        verifyUnchanged(before, assigned);
    }

    void rejectsInvalidSvgWithoutStateChange_data()
    {
        QTest::addColumn<bool>("invalidSvgIsReferenced");

        QTest::newRow("referenced-svg") << true;
        QTest::newRow("unreferenced-svg") << false;
    }

    void rejectsInvalidSvgWithoutStateChange()
    {
        QFETCH(bool, invalidSvgIsReferenced);
        const IconState before = captureIconState();
        const QString assigned = iconManager->assignedIcon("Sport", "Bike");

        BundleEntries entries {
            { "bike.svg",
              invalidSvgIsReferenced ? QByteArray("not svg") : svgData() },
            { "mapping.json", mappingData() },
        };
        if (!invalidSvgIsReferenced)
            entries.prepend({ "unused.svg", "<svg><broken>" });
        const QString archive = createBundle("invalid-svg.zip", entries);
        QVERIFY(!archive.isEmpty());

        QVERIFY(!iconManager->importBundle(archive));
        verifyUnchanged(before, assigned);
    }

    void rejectsExistingDestinationFileLink()
    {
        const IconState before = captureIconState();
        const QString assigned = iconManager->assignedIcon("Sport", "Bike");
        const QString victim = root.filePath("file-link-victim.svg");
        QVERIFY(writeFile(victim, "original"));
        if (!createFileLink(victim, iconPath("bike.svg")))
            QSKIP("File symbolic links are unavailable");

        const QString archive = createBundle(
            "destination-file-link.zip",
            {
                { "bike.svg", svgData() },
                { "mapping.json", mappingData() },
            });
        QVERIFY(!archive.isEmpty());
        const bool imported = iconManager->importBundle(archive);

        QVERIFY(removeFileLink(iconPath("bike.svg")));
        QVERIFY(!imported);
        QCOMPARE(readFile(victim), QByteArray("original"));
        verifyUnchanged(before, assigned);
    }

    void rejectsDestinationDirectoryLink()
    {
        const IconState before = captureIconState();
        const QString assigned = iconManager->assignedIcon("Sport", "Bike");
        const QString outside = root.filePath("destination-link-outside");
        const QString backup = QDir(profileDirectory()).filePath(".icons-real");
        QVERIFY(QDir().mkpath(outside));
        QVERIFY(QDir(profileDirectory()).rename(".icons", ".icons-real"));
#if defined(Q_OS_WIN)
        QVERIFY2(createDirectoryJunction(outside, iconDirectory()),
                 "Creating a directory junction failed");
#else
        if (!createDirectoryLink(outside, iconDirectory())) {
            QVERIFY(QDir(profileDirectory()).rename(".icons-real", ".icons"));
            QSKIP("Directory symbolic links are unavailable");
        }
#endif

#if defined(Q_OS_WIN)
        const DWORD attributes = GetFileAttributesW(
            reinterpret_cast<LPCWSTR>(
                QDir::toNativeSeparators(iconDirectory()).utf16()));
        QVERIFY(attributes != INVALID_FILE_ATTRIBUTES);
        QVERIFY(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
        QCOMPARE(reparseTag(iconDirectory()),
                 static_cast<DWORD>(IO_REPARSE_TAG_MOUNT_POINT));
        QVERIFY(reparseTag(iconDirectory()) != IO_REPARSE_TAG_SYMLINK);
#endif

        const QString archive = createBundle(
            "destination-directory-link.zip",
            {
                { "bike.svg", svgData() },
                { "mapping.json", mappingData() },
            });
        QVERIFY(!archive.isEmpty());
        const bool imported = iconManager->importBundle(archive);

        QVERIFY(removeDirectoryLink(iconDirectory()));
        QVERIFY(QDir(profileDirectory()).rename(".icons-real", ".icons"));
        QVERIFY(!imported);
        QVERIFY(!QFileInfo::exists(QDir(outside).filePath("bike.svg")));
        QVERIFY(!QFileInfo::exists(QDir(outside).filePath("mapping.json")));
        QVERIFY(!QFileInfo::exists(backup));
        verifyUnchanged(before, assigned);
    }

    void rejectsLinkInDestinationAncestor()
    {
        const IconState before = captureIconState();
        const QString assigned = iconManager->assignedIcon("Sport", "Bike");
        const QString profileParent = root.filePath("profile");
        const QString outside = root.filePath("ancestor-link-outside");
        QVERIFY(QDir().mkpath(QDir(outside).filePath(".icons")));
        QVERIFY(QDir(profileParent).rename("current", "current-real"));
        if (!createDirectoryLink(outside, profileDirectory())) {
            QVERIFY(QDir(profileParent).rename("current-real", "current"));
            QSKIP("Directory symbolic links are unavailable");
        }

        const QString archive = createBundle(
            "ancestor-link.zip",
            {
                { "bike.svg", svgData() },
                { "mapping.json", mappingData() },
            });
        QVERIFY(!archive.isEmpty());
        const bool imported = iconManager->importBundle(archive);

        QVERIFY(removeDirectoryLink(profileDirectory()));
        QVERIFY(QDir(profileParent).rename("current-real", "current"));
        QVERIFY(!imported);
        QVERIFY(!QFileInfo::exists(
            QDir(outside).filePath(".icons/bike.svg")));
        verifyUnchanged(before, assigned);
    }

    void rejectsExistingCaseAlias()
    {
        QVERIFY(writeFile(iconPath("BIKE.svg"), svgData("#a23b3b")));
        const IconState before = captureIconState();
        const QString assigned = iconManager->assignedIcon("Sport", "Bike");
        const QString archive = createBundle(
            "destination-alias.zip",
            {
                { "bike.svg", svgData() },
                { "mapping.json", mappingData() },
            });
        QVERIFY(!archive.isEmpty());

        QVERIFY(!iconManager->importBundle(archive));
        verifyUnchanged(before, assigned);
    }

    void rejectsExistingUnicodeAlias()
    {
        const QString composed =
            QString("caf") + QChar(0x00e9) + ".svg";
        const QString decomposed =
            QString("cafe") + QChar(0x0301) + ".svg";
        QVERIFY(writeFile(iconPath(decomposed), svgData("#a23b3b")));
        if (QFileInfo(iconPath(decomposed)).fileName() != decomposed)
            QSKIP("The filesystem normalizes filenames");

        const IconState before = captureIconState();
        const QString assigned = iconManager->assignedIcon("Sport", "Bike");
        const QString archive = createBundle(
            "destination-unicode-alias.zip",
            {
                { composed, svgData() },
                { "mapping.json", mappingData(composed.toUtf8()) },
            });
        QVERIFY(!archive.isEmpty());

        QVERIFY(!iconManager->importBundle(archive));
        verifyUnchanged(before, assigned);
    }

    void revalidatesTargetImmediatelyBeforeCommit()
    {
        const IconState before = captureIconState();
        const QString assigned = iconManager->assignedIcon("Sport", "Bike");
        const QString victim = root.filePath("late-link-victim.svg");
        QVERIFY(writeFile(victim, "original"));
        bool linkCreated = false;
        IconManager::setBundleCommitHookForTest(
            [this, victim, &linkCreated](const QString &name, int) {
                if (name == "run.svg") {
                    linkCreated = createFileLink(victim, iconPath(name));
                    return linkCreated;
                }
                return true;
            });

        const QString archive = createBundle(
            "late-target-link.zip",
            {
                { "bike.svg", svgData() },
                { "run.svg", svgData("#a23b3b") },
                { "mapping.json", mappingData() },
            });
        QVERIFY(!archive.isEmpty());
        const bool imported = iconManager->importBundle(archive);
        IconManager::setBundleCommitHookForTest({});

        if (linkCreated)
            QVERIFY(removeFileLink(iconPath("run.svg")));
        else
            QSKIP("File symbolic links are unavailable");
        QVERIFY(!imported);
        QCOMPARE(readFile(victim), QByteArray("original"));
        verifyUnchanged(before, assigned);
    }

    void revalidatesRootImmediatelyBeforeCommit()
    {
        const IconState before = captureIconState();
        const QString assigned = iconManager->assignedIcon("Sport", "Bike");
        const QString outside = root.filePath("late-root-outside");
        const QString backup =
            QDir(profileDirectory()).filePath(".icons-real");
        QVERIFY(QDir().mkpath(outside));

        bool rootReplaced = false;
        bool validationObserved = false;
        bool validationRejected = false;
        bool restoredDuringValidation = false;
        QString replacementError;
        IconManager::setBundleCommitHookForTest(
            [this, outside, &rootReplaced, &replacementError](
                const QString &name, int committed) {
                if (name != "bike.svg")
                    return true;
                if (committed != 0) {
                    replacementError = "The first file was already committed";
                    return false;
                }
                if (!QDir(profileDirectory()).rename(
                        ".icons", ".icons-real")) {
                    replacementError = "Could not move the destination root";
                    return false;
                }
                if (!createDirectoryRedirection(outside, iconDirectory())) {
                    replacementError =
                        "Could not redirect the destination root";
                    QDir(profileDirectory()).rename(
                        ".icons-real", ".icons");
                    return false;
                }
                rootReplaced = true;
                return true;
            });
        IconManager::setBundleValidationHookForTest(
            [this, &rootReplaced, &validationObserved,
             &validationRejected, &restoredDuringValidation,
             &replacementError](bool targetIsValid) {
                validationObserved = true;
                validationRejected = !targetIsValid;
                if (!rootReplaced)
                    return;
                restoredDuringValidation =
                    removeDirectoryLink(iconDirectory())
                    && QDir(profileDirectory()).rename(
                        ".icons-real", ".icons");
                if (!restoredDuringValidation)
                    replacementError =
                        "Could not restore the destination root";
            });

        const QString archive = createBundle(
            "late-root-link.zip",
            {
                { "bike.svg", svgData() },
                { "mapping.json", mappingData() },
            });
        QVERIFY(!archive.isEmpty());
        const bool imported = iconManager->importBundle(archive);
        IconManager::setBundleCommitHookForTest({});
        IconManager::setBundleValidationHookForTest({});

        if (rootReplaced && !restoredDuringValidation) {
            removeDirectoryLink(iconDirectory());
            QDir(profileDirectory()).rename(".icons-real", ".icons");
        }

        QVERIFY2(rootReplaced, qPrintable(replacementError));
        QVERIFY2(validationObserved,
                 "Final destination validation was not observed");
        QVERIFY2(validationRejected,
                 "Redirected destination root passed final validation");
        QVERIFY2(restoredDuringValidation, qPrintable(replacementError));
        QVERIFY(!imported);
        QVERIFY(!QFileInfo::exists(QDir(outside).filePath("bike.svg")));
        QVERIFY(!QFileInfo::exists(QDir(outside).filePath("mapping.json")));
        QVERIFY(!QFileInfo::exists(backup));
        verifyUnchanged(before, assigned);
    }

    void revalidatesAncestorImmediatelyBeforeCommit()
    {
        const IconState before = captureIconState();
        const QString assigned = iconManager->assignedIcon("Sport", "Bike");
        const QString profileParent = root.filePath("profile");
        const QString outside = root.filePath("late-ancestor-outside");
        const QString backup = QDir(profileParent).filePath("current-real");
        QVERIFY(QDir().mkpath(QDir(outside).filePath(".icons")));

        bool ancestorReplaced = false;
        bool validationObserved = false;
        bool validationRejected = false;
        bool restoredDuringValidation = false;
        QString replacementError;
        IconManager::setBundleCommitHookForTest(
            [this, profileParent, outside, &ancestorReplaced,
             &replacementError](const QString &name, int committed) {
                if (name != "bike.svg")
                    return true;
                if (committed != 0) {
                    replacementError = "The first file was already committed";
                    return false;
                }
                if (!QDir(profileParent).rename(
                        "current", "current-real")) {
                    replacementError =
                        "Could not move the destination ancestor";
                    return false;
                }
                if (!createDirectoryRedirection(
                        outside, profileDirectory())) {
                    replacementError =
                        "Could not redirect the destination ancestor";
                    QDir(profileParent).rename(
                        "current-real", "current");
                    return false;
                }
                ancestorReplaced = true;
                return true;
            });
        IconManager::setBundleValidationHookForTest(
            [this, profileParent, &ancestorReplaced,
             &validationObserved, &validationRejected,
             &restoredDuringValidation,
             &replacementError](bool targetIsValid) {
                validationObserved = true;
                validationRejected = !targetIsValid;
                if (!ancestorReplaced)
                    return;
                restoredDuringValidation =
                    removeDirectoryLink(profileDirectory())
                    && QDir(profileParent).rename(
                        "current-real", "current");
                if (!restoredDuringValidation)
                    replacementError =
                        "Could not restore the destination ancestor";
            });

        const QString archive = createBundle(
            "late-ancestor-link.zip",
            {
                { "bike.svg", svgData() },
                { "mapping.json", mappingData() },
            });
        QVERIFY(!archive.isEmpty());
        const bool imported = iconManager->importBundle(archive);
        IconManager::setBundleCommitHookForTest({});
        IconManager::setBundleValidationHookForTest({});

        if (ancestorReplaced && !restoredDuringValidation) {
            removeDirectoryLink(profileDirectory());
            QDir(profileParent).rename("current-real", "current");
        }

        QVERIFY2(ancestorReplaced, qPrintable(replacementError));
        QVERIFY2(validationObserved,
                 "Final ancestor validation was not observed");
        QVERIFY2(validationRejected,
                 "Redirected destination ancestor passed final validation");
        QVERIFY2(restoredDuringValidation, qPrintable(replacementError));
        QVERIFY(!imported);
        QVERIFY(!QFileInfo::exists(
            QDir(outside).filePath(".icons/bike.svg")));
        QVERIFY(!QFileInfo::exists(
            QDir(outside).filePath(".icons/mapping.json")));
        QVERIFY(!QFileInfo::exists(backup));
        verifyUnchanged(before, assigned);
    }

    void rollsBackAllCommittedFilesOnPromotionFailure()
    {
        const QFile::Permissions restrictedPermissions =
            QFile::ReadOwner | QFile::WriteOwner;
        QVERIFY(QFile::setPermissions(
            iconPath("README.txt"), restrictedPermissions));
        const QFile::Permissions originalPermissions =
            QFileInfo(iconPath("README.txt")).permissions();
        const IconState before = captureIconState();
        const QString assigned = iconManager->assignedIcon("Sport", "Bike");
        QStringList commits;
        IconManager::setBundleCommitHookForTest(
            [&commits](const QString &name, int committed) {
                commits.append(QString("%1:%2").arg(name).arg(committed));
                return name != "mapping.json";
            });

        const QString archive = createBundle(
            "rollback.zip",
            {
                { "bike.svg", svgData() },
                { "run.svg", svgData("#a23b3b") },
                { "README.txt", "replacement license" },
                { "mapping.json", mappingData() },
            });
        QVERIFY(!archive.isEmpty());

        QVERIFY(!iconManager->importBundle(archive));
        IconManager::setBundleCommitHookForTest({});
        QVERIFY2(commits.contains("mapping.json:3"),
                 qPrintable(QString("commits: %1")
                                .arg(commits.join(", "))));
        verifyUnchanged(before, assigned);
        QCOMPARE(QFileInfo(iconPath("README.txt")).permissions(),
                 originalPermissions);
    }

    void rejectsHttpBundle()
    {
        const IconState before = captureIconState();
        const QString assigned = iconManager->assignedIcon("Sport", "Bike");
        const QString archive = createBundle(
            "http.zip",
            {
                { "bike.svg", svgData() },
                { "mapping.json", mappingData() },
            });
        QVERIFY(!archive.isEmpty());
        const QByteArray bundle = readFile(archive);

        int connections = 0;
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        connect(&server, &QTcpServer::newConnection, &server,
                [&server, bundle, &connections]() {
            while (server.hasPendingConnections()) {
                ++connections;
                QTcpSocket *socket = server.nextPendingConnection();
                const QByteArray response =
                    QByteArray("HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\n")
                    + "Content-Length: " + QByteArray::number(bundle.size())
                    + "\r\nConnection: close\r\n\r\n" + bundle;
                socket->write(response);
                socket->disconnectFromHost();
            }
        });

        const QUrl url(QString("http://127.0.0.1:%1/icons.zip")
                           .arg(server.serverPort()));
        const bool imported = iconManager->importBundle(url);
        QTest::qWait(50);

        QVERIFY(!QFileInfo::exists(iconPath("bike.svg")));
        QVERIFY(!imported);
        QCOMPARE(connections, 0);
        verifyUnchanged(before, assigned);
    }

    void importsBundleOverAuthenticatedHttps()
    {
        if (!QSslSocket::supportsSsl())
            QSKIP("The Qt TLS backend is unavailable");

        const QString archive = createBundle(
            "https.zip",
            {
                { "bike.svg", svgData() },
                { "mapping.json", mappingData() },
            });
        QVERIFY(!archive.isEmpty());
        const QByteArray bundle = readFile(archive);
        const QSslCertificate certificate(tlsCertificatePem(), QSsl::Pem);
        const QSslKey key(tlsPrivateKeyPem(), QSsl::Rsa, QSsl::Pem);
        QVERIFY(!certificate.isNull());
        QVERIFY(!key.isNull());

        DefaultSslConfigurationGuard sslGuard;
        QSslConfiguration configuration =
            QSslConfiguration::defaultConfiguration();
        QList<QSslCertificate> authorities = configuration.caCertificates();
        authorities.append(certificate);
        configuration.setCaCertificates(authorities);
        QSslConfiguration::setDefaultConfiguration(configuration);

        TlsHttpServer server(
            certificate,
            key,
            httpResponse(
                "HTTP/1.1 200 OK\r\nContent-Type: application/zip",
                bundle));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QUrl url(QString("https://127.0.0.1:%1/icons.zip")
                           .arg(server.serverPort()));

        QVERIFY(iconManager->importBundle(url));
        QCOMPARE(server.connectionCount, 1);
        QCOMPARE(readFile(iconPath("bike.svg")), svgData());
        QCOMPARE(iconManager->assignedIcon("Sport", "Bike"),
                 QString("bike.svg"));
    }

    void rejectsAuthenticatedHttpsErrorStatus_data()
    {
        QTest::addColumn<int>("statusCode");

        QTest::newRow("not-found") << 404;
        QTest::newRow("server-error") << 500;
    }

    void rejectsAuthenticatedHttpsErrorStatus()
    {
        QFETCH(int, statusCode);
        if (!QSslSocket::supportsSsl())
            QSKIP("The Qt TLS backend is unavailable");

        const IconState before = captureIconState();
        const QString assigned = iconManager->assignedIcon("Sport", "Bike");
        const QSslCertificate certificate(tlsCertificatePem(), QSsl::Pem);
        const QSslKey key(tlsPrivateKeyPem(), QSsl::Rsa, QSsl::Pem);
        QVERIFY(!certificate.isNull());
        QVERIFY(!key.isNull());

        DefaultSslConfigurationGuard sslGuard;
        QSslConfiguration configuration =
            QSslConfiguration::defaultConfiguration();
        QList<QSslCertificate> authorities = configuration.caCertificates();
        authorities.append(certificate);
        configuration.setCaCertificates(authorities);
        QSslConfiguration::setDefaultConfiguration(configuration);

        const QByteArray reason = statusCode == 404
            ? QByteArray(" Not Found")
            : QByteArray(" Internal Server Error");
        TlsHttpServer server(
            certificate,
            key,
            httpResponse(
                "HTTP/1.1 " + QByteArray::number(statusCode) + reason,
                "not a bundle"));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QUrl url(QString("https://127.0.0.1:%1/icons.zip")
                           .arg(server.serverPort()));

        QVERIFY(!iconManager->importBundle(url));
        QCOMPARE(server.connectionCount, 1);
        verifyUnchanged(before, assigned);
    }

    void rejectsHttpsDowngradeRedirect()
    {
        if (!QSslSocket::supportsSsl())
            QSKIP("The Qt TLS backend is unavailable");

        const IconState before = captureIconState();
        const QString assigned = iconManager->assignedIcon("Sport", "Bike");
        const QSslCertificate certificate(tlsCertificatePem(), QSsl::Pem);
        const QSslKey key(tlsPrivateKeyPem(), QSsl::Rsa, QSsl::Pem);
        QVERIFY(!certificate.isNull());
        QVERIFY(!key.isNull());

        DefaultSslConfigurationGuard sslGuard;
        QSslConfiguration configuration =
            QSslConfiguration::defaultConfiguration();
        QList<QSslCertificate> authorities = configuration.caCertificates();
        authorities.append(certificate);
        configuration.setCaCertificates(authorities);
        QSslConfiguration::setDefaultConfiguration(configuration);

        int cleartextConnections = 0;
        QTcpServer cleartextServer;
        QVERIFY(cleartextServer.listen(QHostAddress::LocalHost));
        connect(&cleartextServer, &QTcpServer::newConnection,
                &cleartextServer, [&]() {
            ++cleartextConnections;
            while (cleartextServer.hasPendingConnections()) {
                cleartextServer.nextPendingConnection()->disconnectFromHost();
            }
        });

        const QByteArray location =
            "http://127.0.0.1:"
            + QByteArray::number(cleartextServer.serverPort())
            + "/icons.zip";
        TlsHttpServer server(
            certificate,
            key,
            httpResponse(
                "HTTP/1.1 302 Found\r\nLocation: " + location));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QUrl url(QString("https://127.0.0.1:%1/icons.zip")
                           .arg(server.serverPort()));

        QVERIFY(!iconManager->importBundle(url));
        QCOMPARE(server.connectionCount, 1);
        QCOMPARE(cleartextConnections, 0);
        verifyUnchanged(before, assigned);
    }

    void rejectsTlsCertificateErrors()
    {
        if (!QSslSocket::supportsSsl())
            QSKIP("The Qt TLS backend is unavailable");

        const IconState before = captureIconState();
        const QString assigned = iconManager->assignedIcon("Sport", "Bike");
        const QSslCertificate certificate(tlsCertificatePem(), QSsl::Pem);
        const QSslKey key(tlsPrivateKeyPem(), QSsl::Rsa, QSsl::Pem);
        QVERIFY(!certificate.isNull());
        QVERIFY(!key.isNull());

        TlsHttpServer server(
            certificate,
            key,
            httpResponse("HTTP/1.1 200 OK", "not reached"));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QUrl url(QString("https://127.0.0.1:%1/icons.zip")
                           .arg(server.serverPort()));

        QVERIFY(!iconManager->importBundle(url));
        QCOMPARE(server.connectionCount, 1);
        verifyUnchanged(before, assigned);
    }

    void extractsValidBundle()
    {
        const QString archive = createBundle(
            "valid.zip",
            {
                { "bike.svg", svgData() },
                { "README.txt", "Icon license information" },
                { "mapping.json", mappingData() },
            });
        QVERIFY(!archive.isEmpty());

        QVERIFY(iconManager->importBundle(archive));

        QCOMPARE(readFile(iconPath("bike.svg")), svgData());
        QCOMPARE(readFile(iconPath("README.txt")),
                 QByteArray("Icon license information"));
        QCOMPARE(readFile(iconPath("mapping.json")), mappingData());
        QCOMPARE(iconManager->assignedIcon("Sport", "Bike"),
                 QString("bike.svg"));
    }
};

QTEST_MAIN(TestIconBundleSecurity)
#include "testIconBundleSecurity.moc"
