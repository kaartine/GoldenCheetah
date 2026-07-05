/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "FileIO/ArchiveFile.h"
#include "zipreader.h"
#include "zipwriter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
#endif

class TestArchiveSecurity : public QObject
{
    Q_OBJECT

    static bool writeFile(const QString &fileName, const QByteArray &contents)
    {
        QFile file(fileName);
        return file.open(QIODevice::WriteOnly)
            && file.write(contents) == contents.size();
    }

    static QByteArray readFile(const QString &fileName)
    {
        QFile file(fileName);
        if (!file.open(QIODevice::ReadOnly)) return QByteArray();
        return file.readAll();
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

private slots:

    void rejectsCrossPlatformUnsafeNames_data()
    {
        QTest::addColumn<QString>("memberName");

        QTest::newRow("absolute-file") << QString("/absolute.fit");
        QTest::newRow("drive-path") << QString("C:/victim.fit");
        QTest::newRow("backslash-traversal") << QString("..\\victim.fit");
        QTest::newRow("resource-path") << QString(":/victim.fit");
        QTest::newRow("dot-component") << QString("nested/./victim.fit");
        QTest::newRow("windows-device") << QString("NUL.fit");
        QTest::newRow("windows-trailing-dot") << QString("trailing./victim.fit");
    }

    void rejectsCrossPlatformUnsafeNames()
    {
        QFETCH(QString, memberName);

        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName = sandbox.filePath("unsafe-name.zip");
        const QString destination = sandbox.filePath("extracted");
        QVERIFY(QDir().mkpath(destination));

        ZipWriter writer(archiveName);
        writer.addFile(memberName, "unsafe");
        writer.close();

        ZipReader reader(archiveName);
        QVERIFY(!reader.extractAll(destination));
        QVERIFY(QDir(destination).entryList(
            QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }

    void rejectsTraversalBeforeWriting()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName = sandbox.filePath("traversal.zip");
        const QString destination = sandbox.filePath("extracted");
        const QString victim = sandbox.filePath("victim");
        QVERIFY(QDir().mkpath(destination));
        QVERIFY(writeFile(victim, "original"));

        ZipWriter writer(archiveName);
        QVERIFY(writer.isWritable());
        writer.addFile("safe.fit", "safe");
        writer.addFile("../victim", "overwritten");
        writer.close();
        QCOMPARE(writer.status(), ZipWriter::NoError);

        ZipReader reader(archiveName);
        QVERIFY(!reader.extractAll(destination));
        QCOMPARE(readFile(victim), QByteArray("original"));
        QVERIFY(!QFileInfo::exists(QDir(destination).filePath("safe.fit")));
    }

    void rejectsAbsoluteDirectory()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName = sandbox.filePath("absolute-directory.zip");
        const QString destination = sandbox.filePath("extracted");
        const QString outsideDirectory = sandbox.filePath("outside");
        QVERIFY(QDir().mkpath(destination));

        ZipWriter writer(archiveName);
        writer.addDirectory(outsideDirectory);
        writer.close();

        ZipReader reader(archiveName);
        QVERIFY(!reader.extractAll(destination));
        QVERIFY(!QFileInfo::exists(outsideDirectory));
    }

    void rejectsArchiveSymlinksBeforeWriting()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName = sandbox.filePath("archive-symlink.zip");
        const QString destination = sandbox.filePath("extracted");
        const QString victim = sandbox.filePath("victim");
        QVERIFY(QDir().mkpath(destination));
        QVERIFY(writeFile(victim, "original"));

        ZipWriter writer(archiveName);
        writer.addSymLink("escape", "..");
        writer.addFile("escape/victim", "overwritten");
        writer.close();

        ZipReader reader(archiveName);
        QVERIFY(!reader.extractAll(destination));
        QCOMPARE(readFile(victim), QByteArray("original"));
        QVERIFY(!QFileInfo::exists(QDir(destination).filePath("escape")));
    }

    void rejectsExistingDestinationSymlink()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName = sandbox.filePath("existing-symlink.zip");
        const QString destination = sandbox.filePath("extracted");
        const QString escape = QDir(destination).filePath("escape");
        const QString victim = sandbox.filePath("victim");
        QVERIFY(QDir().mkpath(destination));
        QVERIFY(writeFile(victim, "original"));
        if (!createDirectoryLink(sandbox.path(), escape))
            QSKIP("Directory symbolic links are unavailable");

        ZipWriter writer(archiveName);
        writer.addFile("escape/victim", "overwritten");
        writer.close();

        ZipReader reader(archiveName);
        QVERIFY(!reader.extractAll(destination));
        QCOMPARE(readFile(victim), QByteArray("original"));
    }

    void rejectsExistingDestinationFile()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName = sandbox.filePath("existing-file.zip");
        const QString destination = sandbox.filePath("extracted");
        const QString target = QDir(destination).filePath("activity.fit");
        QVERIFY(QDir().mkpath(destination));
        QVERIFY(writeFile(target, "original"));

        ZipWriter writer(archiveName);
        writer.addFile("activity.fit", "replacement");
        writer.close();

        ZipReader reader(archiveName);
        QVERIFY(!reader.extractAll(destination));
        QCOMPARE(readFile(target), QByteArray("original"));
    }

    void preservesExistingDirectoryPermissions()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName = sandbox.filePath("existing-directory.zip");
        const QString destination = sandbox.filePath("extracted");
        const QString existingDirectory =
            QDir(destination).filePath("existing");
        QVERIFY(QDir().mkpath(existingDirectory));

        const QFile::Permissions originalPermissions =
            QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
        QVERIFY(QFile::setPermissions(existingDirectory, originalPermissions));
        const QFile::Permissions effectivePermissions =
            QFileInfo(existingDirectory).permissions();

        ZipWriter writer(archiveName);
        writer.setCreationPermissions(QFile::ReadOwner | QFile::ExeOwner);
        writer.addDirectory("existing");
        writer.addFile("existing/activity.fit", "activity");
        writer.close();

        ZipReader reader(archiveName);
        const bool extracted = reader.extractAll(destination);
        const QFile::Permissions resultingPermissions =
            QFileInfo(existingDirectory).permissions();

        // Keep QTemporaryDir cleanup reliable even when the assertion fails.
        QFile::setPermissions(existingDirectory, originalPermissions);

        QVERIFY(extracted);
        QCOMPARE(resultingPermissions, effectivePermissions);
        QCOMPARE(readFile(QDir(existingDirectory).filePath("activity.fit")),
                 QByteArray("activity"));
    }

    void rollsBackCreatedDirectoriesAfterWriteFailure()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName = sandbox.filePath("rollback.zip");
        const QString destination = sandbox.filePath("extracted");
        QVERIFY(QDir().mkpath(destination));

        const QString overlongComponent(300, QLatin1Char('x'));
        ZipWriter writer(archiveName);
        writer.addFile("first.fit", "first");
        writer.addFile(
            QString("created/%1/failure.fit").arg(overlongComponent),
            "failure");
        writer.close();

        ZipReader reader(archiveName);
        QVERIFY(!reader.extractAll(destination));
        QVERIFY(QDir(destination).entryList(
            QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }

    void rejectsCorruptDataBeforeWriting()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName = sandbox.filePath("corrupt.zip");
        const QString destination = sandbox.filePath("extracted");
        QVERIFY(QDir().mkpath(destination));

        ZipWriter writer(archiveName);
        writer.setCompressionPolicy(ZipWriter::NeverCompress);
        writer.addFile("safe.fit", "safe");
        writer.addFile("corrupt.fit", "corrupt-me");
        writer.close();

        QFile archive(archiveName);
        QVERIFY(archive.open(QIODevice::ReadWrite));
        QByteArray bytes = archive.readAll();
        const qsizetype payload = bytes.indexOf("corrupt-me");
        QVERIFY(payload >= 0);
        bytes[payload] = 'C';
        QVERIFY(archive.seek(0));
        QCOMPARE(archive.write(bytes), bytes.size());
        archive.close();

        ZipReader reader(archiveName);
        QVERIFY(!reader.extractAll(destination));
        QVERIFY(!QFileInfo::exists(QDir(destination).filePath("safe.fit")));
        QVERIFY(!QFileInfo::exists(QDir(destination).filePath("corrupt.fit")));
    }

    void rejectsIncompleteCentralDirectoryBeforeWriting()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName = sandbox.filePath("central-directory.zip");
        const QString destination = sandbox.filePath("extracted");
        QVERIFY(QDir().mkpath(destination));

        ZipWriter writer(archiveName);
        writer.addFile("first.fit", "first");
        writer.addFile("second.fit", "second");
        writer.close();

        QFile archive(archiveName);
        QVERIFY(archive.open(QIODevice::ReadOnly));
        QByteArray bytes = archive.readAll();
        archive.close();

        const QByteArray centralHeader = QByteArray::fromHex("504b0102");
        const qsizetype firstHeader = bytes.indexOf(centralHeader);
        const qsizetype secondHeader =
            bytes.indexOf(centralHeader, firstHeader + centralHeader.size());
        QVERIFY(firstHeader >= 0);
        QVERIFY(secondHeader > firstHeader);
        bytes[secondHeader] = 'X';
        QVERIFY(writeFile(archiveName, bytes));

        ZipReader reader(archiveName);
        QVERIFY(!reader.extractAll(destination));
        QCOMPARE(reader.status(), ZipReader::FileReadError);
        QVERIFY(QDir(destination).entryList(
            QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }

    void rejectsCentralDirectoryCountMismatch_data()
    {
        QTest::addColumn<bool>("shrinkDirectorySize");

        QTest::newRow("entry-count-only") << false;
        QTest::newRow("entry-count-and-size") << true;
    }

    void rejectsCentralDirectoryCountMismatch()
    {
        QFETCH(bool, shrinkDirectorySize);

        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName = sandbox.filePath("entry-count.zip");
        const QString destination = sandbox.filePath("extracted");
        QVERIFY(QDir().mkpath(destination));

        ZipWriter writer(archiveName);
        writer.addFile("first.fit", "first");
        writer.addFile("second.fit", "second");
        writer.close();

        QFile archive(archiveName);
        QVERIFY(archive.open(QIODevice::ReadOnly));
        QByteArray bytes = archive.readAll();
        archive.close();

        const QByteArray centralHeader = QByteArray::fromHex("504b0102");
        const QByteArray endHeader = QByteArray::fromHex("504b0506");
        const qsizetype firstHeader = bytes.indexOf(centralHeader);
        const qsizetype secondHeader =
            bytes.indexOf(centralHeader, firstHeader + centralHeader.size());
        const qsizetype endOfDirectory = bytes.lastIndexOf(endHeader);
        QVERIFY(firstHeader >= 0);
        QVERIFY(secondHeader > firstHeader);
        QVERIFY(endOfDirectory > secondHeader);

        bytes[endOfDirectory + 8] = 1;
        bytes[endOfDirectory + 9] = 0;
        bytes[endOfDirectory + 10] = 1;
        bytes[endOfDirectory + 11] = 0;
        if (shrinkDirectorySize) {
            const quint32 firstEntrySize =
                static_cast<quint32>(secondHeader - firstHeader);
            for (int byte = 0; byte < 4; ++byte) {
                bytes[endOfDirectory + 12 + byte] =
                    static_cast<char>((firstEntrySize >> (8 * byte)) & 0xff);
            }
        }
        QVERIFY(writeFile(archiveName, bytes));

        ZipReader reader(archiveName);
        QVERIFY(!reader.extractAll(destination));
        QCOMPARE(reader.status(), ZipReader::FileReadError);
        QVERIFY(QDir(destination).entryList(
            QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }

    void extractsOnlyRequestedArchiveMembers()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName = sandbox.filePath("selective.zip");
        const QString destination = sandbox.filePath("extracted");
        QVERIFY(QDir().mkpath(destination));

        ZipWriter writer(archiveName);
        writer.addFile("requested.fit", "requested");
        writer.addFile("unrequested.fit", "unrequested");
        writer.close();

        const QStringList extracted = Archive::extract(
            archiveName,
            QList<QString>() << "requested.fit",
            destination);

        QCOMPARE(extracted,
                 QStringList() << QDir(destination).filePath("requested.fit"));
        QCOMPARE(readFile(extracted.first()), QByteArray("requested"));
        QVERIFY(!QFileInfo::exists(
            QDir(destination).filePath("unrequested.fit")));
    }

    void rejectsCaseMismatchedRequestedMemberWithoutWriting()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName = sandbox.filePath("case-mismatch.zip");
        const QString destination = sandbox.filePath("extracted");
        QVERIFY(QDir().mkpath(destination));

        ZipWriter writer(archiveName);
        writer.addFile("ACTIVITY.fit", "unexpected");
        writer.close();

        const QStringList extracted = Archive::extract(
            archiveName,
            QList<QString>() << "activity.fit",
            destination);

        QVERIFY(extracted.isEmpty());
        QVERIFY(QDir(destination).entryList(
            QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }

    void returnsNormalizedRequestedMember()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName = sandbox.filePath("unicode.zip");
        const QString destination = sandbox.filePath("extracted");
        const QString composed = QStringLiteral("caf\u00e9.fit");
        const QString decomposed = QStringLiteral("cafe\u0301.fit");
        QVERIFY(QDir().mkpath(destination));

        ZipWriter writer(archiveName);
        writer.addFile(composed, "activity");
        writer.close();

        const QStringList extracted = Archive::extract(
            archiveName,
            QList<QString>() << decomposed,
            destination);

        QCOMPARE(extracted.size(), 1);
        QCOMPARE(QFileInfo(extracted.first()).fileName(), composed);
        QCOMPARE(readFile(extracted.first()), QByteArray("activity"));
    }

    void rejectsSymlinkInDestinationRoot()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName = sandbox.filePath("symlink-root.zip");
        const QString outsideDirectory = sandbox.filePath("outside");
        const QString linkedRoot = sandbox.filePath("linked-root");
        const QString destination = QDir(linkedRoot).filePath("extracted");
        QVERIFY(QDir().mkpath(outsideDirectory));
        if (!createDirectoryLink(outsideDirectory, linkedRoot))
            QSKIP("Directory symbolic links are unavailable");

        ZipWriter writer(archiveName);
        writer.addFile("safe.fit", "safe");
        writer.close();

        ZipReader reader(archiveName);
        QVERIFY(!reader.extractAll(destination));
        QVERIFY(!QFileInfo::exists(
            QDir(outsideDirectory).filePath("extracted/safe.fit")));
    }

    void extractsValidNestedFiles()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName = sandbox.filePath("valid.zip");
        const QString destination = sandbox.filePath("extracted");
        QVERIFY(QDir().mkpath(destination));

        ZipWriter writer(archiveName);
        writer.setCreationPermissions(
            QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        writer.addDirectory("nested");
        writer.addFile("nested/activity.fit", "activity");
        writer.close();

        ZipReader reader(archiveName);
        QStringList extracted;
        QVERIFY(reader.extractAll(destination, &extracted));
        QCOMPARE(extracted, QStringList() << "nested/activity.fit");
        QCOMPARE(readFile(QDir(destination).filePath("nested/activity.fit")),
                 QByteArray("activity"));
    }

    void archiveWrapperReportsUnsafeArchiveAsFailure()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName = sandbox.filePath("wrapper.zip");
        const QString destination = sandbox.filePath("extracted");
        const QString victim = sandbox.filePath("victim");
        QVERIFY(QDir().mkpath(destination));
        QVERIFY(writeFile(victim, "original"));

        ZipWriter writer(archiveName);
        writer.addFile("safe.fit", "safe");
        writer.addFile("../victim", "overwritten");
        writer.close();

        const QStringList extracted = Archive::extract(
            archiveName,
            QList<QString>() << "safe.fit" << "../victim",
            destination);

        QVERIFY(extracted.isEmpty());
        QCOMPARE(readFile(victim), QByteArray("original"));
        QVERIFY(!QFileInfo::exists(QDir(destination).filePath("safe.fit")));
    }
};

QTEST_MAIN(TestArchiveSecurity)
#include "testArchiveSecurity.moc"
