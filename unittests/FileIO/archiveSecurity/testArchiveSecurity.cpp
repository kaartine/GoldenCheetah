/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "FileIO/ArchiveFile.h"
#include "FileIO/CompressedActivityFile.h"
#include "zipreader.h"
#include "zipwriter.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#ifdef Q_CC_MSVC
#include <QtZlib/zlib.h>
#else
#include <zlib.h>
#endif

namespace {

QByteArray deterministicData(qsizetype size)
{
    QByteArray data(size, Qt::Uninitialized);
    quint32 state = 0x9e3779b9U;
    for (qsizetype index = 0; index < data.size(); ++index) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        data[index] = static_cast<char>(state & 0xff);
    }
    return data;
}

QByteArray gzipPayload(const QByteArray &source)
{
    z_stream stream = {};
    if (deflateInit2(
            &stream, Z_BEST_COMPRESSION, Z_DEFLATED,
            MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return {};
    }

    QByteArray output(
        static_cast<qsizetype>(compressBound(uLong(source.size())) + 32),
        Qt::Uninitialized);
    stream.avail_in = static_cast<uInt>(source.size());
    stream.next_in = reinterpret_cast<Bytef *>(
        const_cast<char *>(source.constData()));
    stream.avail_out = static_cast<uInt>(output.size());
    stream.next_out = reinterpret_cast<Bytef *>(output.data());

    const int result = deflate(&stream, Z_FINISH);
    const qsizetype outputSize =
        static_cast<qsizetype>(stream.total_out);
    deflateEnd(&stream);
    if (result != Z_STREAM_END)
        return {};

    output.resize(outputSize);
    return output;
}

class RecordingWriteDevice final : public QIODevice
{
public:
    RecordingWriteDevice()
    {
        open(QIODevice::WriteOnly);
    }

    const QByteArray &data() const { return data_; }
    qint64 maximumWriteSize() const { return maximumWriteSize_; }

protected:
    qint64 readData(char *, qint64) override { return -1; }

    qint64 writeData(const char *data, qint64 size) override
    {
        maximumWriteSize_ = qMax(maximumWriteSize_, size);
        data_.append(data, static_cast<int>(size));
        return size;
    }

private:
    QByteArray data_;
    qint64 maximumWriteSize_ = 0;
};

ArchiveResourceLimits permissiveTestLimits()
{
    ArchiveResourceLimits limits;
    limits.maximumEntries = 16;
    limits.maximumEntrySize = 16 * 1024 * 1024;
    limits.maximumTotalSize = 32 * 1024 * 1024;
    limits.maximumCompressedSize = 32 * 1024 * 1024;
    limits.maximumMetadataSize = 1024 * 1024;
    limits.maximumCompressionRatio = 2048;
    return limits;
}

std::unique_ptr<QIODevice> compressedActivitySource(
    const QString &format,
    const QByteArray &contents,
    bool addSecondFile = false)
{
    QByteArray compressed;
    if (format == QStringLiteral("zip")) {
        QBuffer output(&compressed);
        if (!output.open(QIODevice::WriteOnly)) return {};
        ZipWriter writer(&output);
        writer.setCompressionPolicy(ZipWriter::AlwaysCompress);
        writer.addFile(QStringLiteral("activity.fit"), contents);
        if (addSecondFile)
            writer.addFile(QStringLiteral("second.fit"), contents);
        writer.close();
        if (writer.status() != ZipWriter::NoError) return {};
    } else {
        compressed = gzipPayload(contents);
        if (compressed.isEmpty()) return {};
    }

    auto source = std::make_unique<QBuffer>();
    source->setData(compressed);
    if (!source->open(QIODevice::ReadOnly)) return {};
    return source;
}

} // namespace

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

    static void writeLittleEndian32(
        QByteArray &data,
        qsizetype offset,
        quint32 value)
    {
        for (int byte = 0; byte < 4; ++byte) {
            data[offset + byte] = static_cast<char>(
                (value >> (8 * byte)) & 0xff);
        }
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

    void rejectsExcessiveArchiveEntryCount()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName = sandbox.filePath("too-many-entries.zip");
        ZipWriter writer(archiveName);
        writer.setCompressionPolicy(ZipWriter::NeverCompress);
        for (int index = 0; index < 3; ++index)
            writer.addFile(QString::number(index), QByteArray());
        writer.close();
        QCOMPARE(writer.status(), ZipWriter::NoError);

        ZipReader reader(archiveName);
        ArchiveResourceLimits limits = permissiveTestLimits();
        limits.maximumEntries = 2;
        QVERIFY(reader.setResourceLimits(limits));
        QVERIFY(reader.fileInfoList().isEmpty());
        QCOMPARE(reader.status(), ZipReader::FileReadError);
    }

    void rejectsDeclaredEntryBeyondSizeLimit()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName = sandbox.filePath("oversized-entry.zip");
        ZipWriter writer(archiveName);
        writer.setCompressionPolicy(ZipWriter::NeverCompress);
        writer.addFile("activity.fit", "small");
        writer.close();

        ZipReader reader(archiveName);
        ArchiveResourceLimits limits = permissiveTestLimits();
        limits.maximumEntrySize = 4;
        QVERIFY(reader.setResourceLimits(limits));
        QVERIFY(reader.fileInfoList().isEmpty());
        QCOMPARE(reader.status(), ZipReader::FileReadError);
    }

    void rejectsDeclaredTotalOutputBeyondLimit()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName = sandbox.filePath("oversized-total.zip");
        ZipWriter writer(archiveName);
        writer.setCompressionPolicy(ZipWriter::NeverCompress);
        writer.addFile("first.fit", "small");
        writer.addFile("second.fit", "small");
        writer.close();

        ZipReader reader(archiveName);
        ArchiveResourceLimits limits = permissiveTestLimits();
        limits.maximumEntrySize = 5;
        limits.maximumTotalSize = 9;
        QVERIFY(reader.setResourceLimits(limits));
        QVERIFY(reader.fileInfoList().isEmpty());
        QCOMPARE(reader.status(), ZipReader::FileReadError);
    }

    void rejectsDeclaredCompressedInputBeyondLimit()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName =
            sandbox.filePath("oversized-compressed-input.zip");
        ZipWriter writer(archiveName);
        writer.setCompressionPolicy(ZipWriter::NeverCompress);
        writer.addFile("first.fit", "small");
        writer.addFile("second.fit", "small");
        writer.close();

        ZipReader reader(archiveName);
        ArchiveResourceLimits limits = permissiveTestLimits();
        limits.maximumCompressedSize = 9;
        QVERIFY(reader.setResourceLimits(limits));
        QVERIFY(reader.fileInfoList().isEmpty());
        QCOMPARE(reader.status(), ZipReader::FileReadError);
    }

    void rejectsExcessiveArchiveMetadata()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName =
            sandbox.filePath("oversized-metadata.zip");
        ZipWriter writer(archiveName);
        writer.setCompressionPolicy(ZipWriter::NeverCompress);
        writer.addFile(QString(128, QLatin1Char('n')), "small");
        writer.close();

        ZipReader reader(archiveName);
        ArchiveResourceLimits limits = permissiveTestLimits();
        limits.maximumMetadataSize = 64;
        QVERIFY(reader.setResourceLimits(limits));
        QVERIFY(reader.fileInfoList().isEmpty());
        QCOMPARE(reader.status(), ZipReader::FileReadError);
    }

    void rejectsExcessiveCompressionRatio()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName = sandbox.filePath("high-ratio.zip");
        const QByteArray contents(64 * 1024, '\0');
        ZipWriter writer(archiveName);
        writer.setCompressionPolicy(ZipWriter::AlwaysCompress);
        writer.addFile("activity.fit", contents);
        writer.close();

        ZipReader reader(archiveName);
        ArchiveResourceLimits limits = permissiveTestLimits();
        limits.maximumCompressionRatio = 2;
        QVERIFY(reader.setResourceLimits(limits));
        QVERIFY(reader.fileInfoList().isEmpty());
        QCOMPARE(reader.status(), ZipReader::FileReadError);
    }

    void streamsZipMemberToDestination()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName = sandbox.filePath("streaming.zip");
        const QByteArray contents = deterministicData(2 * 1024 * 1024);
        ZipWriter writer(archiveName);
        writer.setCompressionPolicy(ZipWriter::NeverCompress);
        writer.addFile("activity.fit", contents);
        writer.close();

        ZipReader reader(archiveName);
        RecordingWriteDevice destination;
        QVERIFY(reader.extractFile("activity.fit", &destination));
        QCOMPARE(destination.data(), contents);
        QVERIFY(destination.maximumWriteSize() > 0);
        QVERIFY(destination.maximumWriteSize() <= 64 * 1024);
    }

    void extractsZipUsingDataDescriptor()
    {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());

        const QString archiveName =
            sandbox.filePath("data-descriptor.zip");
        const QByteArray contents("activity");
        ZipWriter writer(archiveName);
        writer.setCompressionPolicy(ZipWriter::NeverCompress);
        writer.addFile("activity.fit", contents);
        writer.close();
        QCOMPARE(writer.status(), ZipWriter::NoError);

        QByteArray bytes = readFile(archiveName);
        const QByteArray centralSignature =
            QByteArray::fromHex("504b0102");
        const QByteArray endSignature =
            QByteArray::fromHex("504b0506");
        const qsizetype centralOffset =
            bytes.indexOf(centralSignature);
        const qsizetype endOffset = bytes.lastIndexOf(endSignature);
        QVERIFY(centralOffset > 0);
        QVERIFY(endOffset > centralOffset);

        bytes[6] = static_cast<char>(
            static_cast<uchar>(bytes.at(6)) | 0x08);
        bytes[centralOffset + 8] = static_cast<char>(
            static_cast<uchar>(bytes.at(centralOffset + 8)) | 0x08);
        for (qsizetype offset = 14; offset < 26; ++offset)
            bytes[offset] = '\0';

        QByteArray descriptor = QByteArray::fromHex("504b0708");
        descriptor.append(bytes.mid(centralOffset + 16, 12));
        QCOMPARE(descriptor.size(), 16);
        bytes.insert(centralOffset, descriptor);

        const qsizetype updatedEndOffset = endOffset + descriptor.size();
        writeLittleEndian32(
            bytes,
            updatedEndOffset + 16,
            static_cast<quint32>(centralOffset + descriptor.size()));
        QVERIFY(writeFile(archiveName, bytes));

        ZipReader reader(archiveName);
        RecordingWriteDevice destination;
        QVERIFY(reader.extractFile("activity.fit", &destination));
        QCOMPARE(destination.data(), contents);
    }

    void streamsValidGzipToDestination()
    {
        const QByteArray contents = deterministicData(2 * 1024 * 1024);
        const QByteArray compressed = gzipPayload(contents);
        QVERIFY(!compressed.isEmpty());

        QBuffer source;
        source.setData(compressed);
        QVERIFY(source.open(QIODevice::ReadOnly));
        RecordingWriteDevice destination;

        QVERIFY(GzipReader::uncompress(
            &source, &destination, permissiveTestLimits()));
        QCOMPARE(destination.data(), contents);
        QVERIFY(destination.maximumWriteSize() > 0);
        QVERIFY(destination.maximumWriteSize() <= 64 * 1024);
    }

    void rejectsGzipBeyondOutputLimit()
    {
        const QByteArray compressed = gzipPayload(QByteArray(2048, 'x'));
        QVERIFY(!compressed.isEmpty());
        QBuffer source;
        source.setData(compressed);
        QVERIFY(source.open(QIODevice::ReadOnly));
        RecordingWriteDevice destination;
        ArchiveResourceLimits limits = permissiveTestLimits();
        limits.maximumEntrySize = 1024;

        QVERIFY(!GzipReader::uncompress(&source, &destination, limits));
    }

    void rejectsGzipBeyondCompressionRatio()
    {
        const QByteArray compressed =
            gzipPayload(QByteArray(64 * 1024, '\0'));
        QVERIFY(!compressed.isEmpty());
        QBuffer source;
        source.setData(compressed);
        QVERIFY(source.open(QIODevice::ReadOnly));
        RecordingWriteDevice destination;
        ArchiveResourceLimits limits = permissiveTestLimits();
        limits.maximumCompressionRatio = 2;

        QVERIFY(!GzipReader::uncompress(&source, &destination, limits));
    }

    void rejectsGzipBeyondCompressedInputLimit()
    {
        const QByteArray compressed =
            gzipPayload(deterministicData(2048));
        QVERIFY(compressed.size() > 128);
        QBuffer source;
        source.setData(compressed);
        QVERIFY(source.open(QIODevice::ReadOnly));
        RecordingWriteDevice destination;
        ArchiveResourceLimits limits = permissiveTestLimits();
        limits.maximumCompressedSize = 128;

        QVERIFY(!GzipReader::uncompress(&source, &destination, limits));
    }

    void rejectsGzipWithCorruptChecksum()
    {
        QByteArray compressed = gzipPayload(QByteArray("activity"));
        QVERIFY(compressed.size() > 8);
        const qsizetype checksumOffset = compressed.size() - 8;
        compressed[checksumOffset] = static_cast<char>(
            compressed.at(checksumOffset) ^ 0x5a);
        QBuffer source;
        source.setData(compressed);
        QVERIFY(source.open(QIODevice::ReadOnly));
        RecordingWriteDevice destination;

        QVERIFY(!GzipReader::uncompress(
            &source, &destination, permissiveTestLimits()));
    }

    void rejectsTrailingGzipData()
    {
        QByteArray compressed = gzipPayload(QByteArray("activity"));
        QVERIFY(!compressed.isEmpty());
        compressed.append("trailing");
        QBuffer source;
        source.setData(compressed);
        QVERIFY(source.open(QIODevice::ReadOnly));
        RecordingWriteDevice destination;

        QVERIFY(!GzipReader::uncompress(
            &source, &destination, permissiveTestLimits()));
    }

    void extractsSingleCompressedActivity_data()
    {
        QTest::addColumn<QString>("format");

        QTest::newRow("zip") << QStringLiteral("zip");
        QTest::newRow("gzip") << QStringLiteral("gzip");
    }

    void extractsSingleCompressedActivity()
    {
        QFETCH(QString, format);
        const QByteArray contents = deterministicData(2 * 1024 * 1024);
        std::unique_ptr<QIODevice> source =
            compressedActivitySource(format, contents);
        QVERIFY(source);
        RecordingWriteDevice destination;
        const CompressedActivityFile::Format archiveFormat =
            format == QStringLiteral("zip")
                ? CompressedActivityFile::Format::Zip
                : CompressedActivityFile::Format::Gzip;

        QVERIFY(CompressedActivityFile::extractSingleFile(
            std::move(source), archiveFormat, &destination));
        QCOMPARE(destination.data(), contents);
        QVERIFY(destination.maximumWriteSize() > 0);
        QVERIFY(destination.maximumWriteSize() <= 64 * 1024);
    }

    void extractsZipActivityWithDosAttributes()
    {
        const QByteArray contents("activity");
        QByteArray compressed;
        QBuffer output(&compressed);
        QVERIFY(output.open(QIODevice::WriteOnly));
        ZipWriter writer(&output);
        writer.setCompressionPolicy(ZipWriter::NeverCompress);
        writer.addFile(QStringLiteral("activity.fit"), contents);
        writer.close();
        QCOMPARE(writer.status(), ZipWriter::NoError);

        const qsizetype centralOffset = compressed.indexOf(
            QByteArray::fromHex("504b0102"));
        QVERIFY(centralOffset > 0);
        QVERIFY(centralOffset + 42 <= compressed.size());

        // A DOS-created ZIP has no Unix file type in the upper attributes.
        compressed[centralOffset + 4] = 20;
        compressed[centralOffset + 5] = 0;
        for (qsizetype offset = 38; offset < 42; ++offset)
            compressed[centralOffset + offset] = 0;
        compressed[centralOffset + 38] = 0x20;

        auto source = std::make_unique<QBuffer>();
        source->setData(compressed);
        QVERIFY(source->open(QIODevice::ReadOnly));
        RecordingWriteDevice destination;

        QVERIFY(CompressedActivityFile::extractSingleFile(
            std::move(source),
            CompressedActivityFile::Format::Zip,
            &destination));
        QCOMPARE(destination.data(), contents);
    }

    void rejectsMultipleZipActivityMembers()
    {
        std::unique_ptr<QIODevice> source = compressedActivitySource(
            QStringLiteral("zip"), QByteArray("activity"), true);
        QVERIFY(source);
        RecordingWriteDevice destination;

        QVERIFY(!CompressedActivityFile::extractSingleFile(
            std::move(source),
            CompressedActivityFile::Format::Zip,
            &destination));
        QVERIFY(destination.data().isEmpty());
    }

    void rejectsHighRatioCompressedActivity_data()
    {
        extractsSingleCompressedActivity_data();
    }

    void rejectsHighRatioCompressedActivity()
    {
        QFETCH(QString, format);
        std::unique_ptr<QIODevice> source = compressedActivitySource(
            format, QByteArray(4 * 1024 * 1024, '\0'));
        QVERIFY(source);
        RecordingWriteDevice destination;
        const CompressedActivityFile::Format archiveFormat =
            format == QStringLiteral("zip")
                ? CompressedActivityFile::Format::Zip
                : CompressedActivityFile::Format::Gzip;

        QVERIFY(!CompressedActivityFile::extractSingleFile(
            std::move(source), archiveFormat, &destination));
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
