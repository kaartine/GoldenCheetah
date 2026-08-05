/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include "LibraryParserSerializeTestStubs.h"

QList<Library *> libraries;

namespace {

bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(contents) == contents.size()
        && file.flush();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

} // namespace

class TestLibraryParserSerialize : public QObject
{
    Q_OBJECT

private slots:
    void cleanup();
    void writesLibraryMetadata();
#ifdef Q_OS_UNIX
    void writeFailurePreservesExistingMetadata();
#endif
};

void TestLibraryParserSerialize::cleanup()
{
    libraries.clear();
}

void TestLibraryParserSerialize::writesLibraryMetadata()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString athletePath = root.filePath(QStringLiteral("athlete"));
    QVERIFY(QDir().mkpath(athletePath));

    Library media;
    media.name = QStringLiteral("Media Library");
    media.paths = {QStringLiteral("/workouts")};
    media.refs = {QStringLiteral("/media/video.mp4")};
    libraries.append(&media);

    QString error;
    QVERIFY2(LibraryParser::serialize(QDir(athletePath), &error),
             qPrintable(error));
    QVERIFY(error.isEmpty());
    const QByteArray metadata =
        readFile(root.filePath(QStringLiteral("library.xml")));
    QVERIFY(metadata.contains("<library name=\"Media Library\">"));
    QVERIFY(metadata.contains("<path>/workouts</path>"));
    QVERIFY(metadata.contains("<ref>/media/video.mp4</ref>"));
}

#ifdef Q_OS_UNIX
void TestLibraryParserSerialize::writeFailurePreservesExistingMetadata()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString athletePath = root.filePath(QStringLiteral("athlete"));
    const QString metadataPath = root.filePath(QStringLiteral("library.xml"));
    QVERIFY(QDir().mkpath(athletePath));
    QVERIFY(writeFile(metadataPath, QByteArrayLiteral("existing metadata")));

    Library media;
    media.name = QStringLiteral("Media Library");
    media.refs = {QStringLiteral("/media/new.mp4")};
    libraries.append(&media);

    const QFileDevice::Permissions originalPermissions =
        QFileInfo(root.path()).permissions();
    QVERIFY(QFile::setPermissions(
        root.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    QString error;
    const bool serialized = LibraryParser::serialize(QDir(athletePath), &error);
    QVERIFY(QFile::setPermissions(root.path(), originalPermissions));

    QVERIFY(!serialized);
    QVERIFY(!error.isEmpty());
    QCOMPARE(readFile(metadataPath), QByteArrayLiteral("existing metadata"));
}
#endif

QTEST_GUILESS_MAIN(TestLibraryParserSerialize)
#include "testLibraryParserSerialize.moc"
