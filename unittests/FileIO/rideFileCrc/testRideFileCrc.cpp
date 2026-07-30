/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "FileIO/RideFileCRC.h"

#include <QBuffer>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

class ControlledDevice : public QIODevice
{
public:
    explicit ControlledDevice(QByteArray bytes)
        : bytes_(std::move(bytes)),
          reportedSize_(bytes_.size())
    {
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }

    bool isSequential() const override
    {
        return false;
    }

    qint64 size() const override
    {
        return reportedSize_;
    }

    bool seek(qint64 position) override
    {
        if (position < 0 || position > reportedSize_)
            return false;
        position_ = position;
        return QIODevice::seek(position);
    }

    void setMaximumRead(qint64 maximumRead)
    {
        maximumRead_ = maximumRead;
    }

    void setReportedSize(qint64 reportedSize)
    {
        reportedSize_ = reportedSize;
    }

    void setFailOnRead(int readCall)
    {
        failOnRead_ = readCall;
    }

    void setAfterRead(
        std::function<void(ControlledDevice &, int)> afterRead)
    {
        afterRead_ = std::move(afterRead);
    }

    void append(const QByteArray &bytes)
    {
        bytes_.append(bytes);
    }

    qint64 maximumRequested() const
    {
        return maximumRequested_;
    }

protected:
    qint64 readData(char *data, qint64 maximumSize) override
    {
        ++readCalls_;
        maximumRequested_ =
            std::max(maximumRequested_, maximumSize);
        if (readCalls_ == failOnRead_)
            return -1;

        const qint64 available =
            static_cast<qint64>(bytes_.size()) - position_;
        const qint64 count = std::min(
            {maximumSize, maximumRead_, available});
        if (count <= 0)
            return 0;

        std::memcpy(
            data,
            bytes_.constData() + position_,
            static_cast<size_t>(count));
        position_ += count;
        if (afterRead_)
            afterRead_(*this, readCalls_);
        return count;
    }

    qint64 writeData(const char *, qint64) override
    {
        return -1;
    }

private:
    QByteArray bytes_;
    qint64 reportedSize_;
    qint64 position_ = 0;
    qint64 maximumRead_ =
        std::numeric_limits<qint64>::max();
    qint64 maximumRequested_ = 0;
    int failOnRead_ = -1;
    int readCalls_ = 0;
    std::function<void(ControlledDevice &, int)> afterRead_;
};

class GeneratedZeroDevice : public QIODevice
{
public:
    explicit GeneratedZeroDevice(qint64 size)
        : size_(size)
    {
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }

    bool isSequential() const override
    {
        return false;
    }

    qint64 size() const override
    {
        return size_;
    }

    bool seek(qint64 position) override
    {
        if (position < 0 || position > size_)
            return false;
        position_ = position;
        return QIODevice::seek(position);
    }

    qint64 maximumRequested() const
    {
        return maximumRequested_;
    }

    qint64 totalRead() const
    {
        return totalRead_;
    }

    int readCalls() const
    {
        return readCalls_;
    }

protected:
    qint64 readData(char *data, qint64 maximumSize) override
    {
        ++readCalls_;
        maximumRequested_ =
            std::max(maximumRequested_, maximumSize);
        const qint64 count =
            std::min(maximumSize, size_ - position_);
        if (count <= 0)
            return 0;
        std::memset(data, 0, static_cast<size_t>(count));
        position_ += count;
        totalRead_ += count;
        return count;
    }

    qint64 writeData(const char *, qint64) override
    {
        return -1;
    }

private:
    qint64 size_;
    qint64 position_ = 0;
    qint64 maximumRequested_ = 0;
    qint64 totalRead_ = 0;
    int readCalls_ = 0;
};

bool compute(const QByteArray &bytes, quint16 &checksum)
{
    QBuffer input;
    input.setData(bytes);
    if (!input.open(QIODevice::ReadOnly))
        return false;
    return RideFileCRC::compute(input, checksum);
}

bool computeFingerprint(
    const QByteArray &bytes,
    RideFileCRC::ContentFingerprint &fingerprint)
{
    QBuffer input;
    input.setData(bytes);
    if (!input.open(QIODevice::ReadOnly))
        return false;
    return RideFileCRC::computeFingerprint(
        input, fingerprint);
}

QByteArray bytesForValue(quint32 value)
{
    QByteArray bytes(4, '\0');
    bytes[0] = static_cast<char>(value & 0xff);
    bytes[1] = static_cast<char>((value >> 8) & 0xff);
    bytes[2] = static_cast<char>((value >> 16) & 0xff);
    bytes[3] = static_cast<char>((value >> 24) & 0xff);
    return bytes;
}

} // namespace

class TestRideFileCrc : public QObject
{
    Q_OBJECT

private slots:
    void matchesStandardVectors_data();
    void matchesStandardVectors();
    void matchesQtChecksum_data();
    void matchesQtChecksum();
    void acceptsPartialReads();
    void rejectsPrematureEndOfFile();
    void rejectsReadFailure();
    void rejectsGrowth();
    void rejectsShrink();
    void rejectsChangedSnapshot();
    void emptySourceStillValidatesSnapshot();
    void boundsEveryReadRequest();
    void rejectsOversizedSourceBeforeReading();
    void restoresInputPosition();
    void rejectsUnavailableInput();
    void windowsIdentityFallbackPolicy();
    void fileSnapshotAcceptsStableFile();
    void fileSnapshotRejectsAtomicReplacement();
    void fileSnapshotRejectsRestoredMtimeOverwrite();
    void fileSnapshotRejectsRemoval();
    void fileSnapshotRejectsGrowthAfterFirstChunk();
    void fileSnapshotRejectsShrinkAfterFirstChunk();
    void fileSnapshotRejectsNonRegularInput();
    void fileSnapshotRejectsFifoWithoutBlocking();
    void fingerprintFailureLeavesOutputUnchanged();
    void captureProvidesStableStagedBytes();
    void captureFailureLeavesOutputUnchanged();
    void fingerprintDistinguishesSameSizeContent();
    void fingerprintDistinguishesCrc16Collision();
    void captureRejectsSourceRewriteAfterCopy();
    void captureRejectsAtomicReplacement();
    void captureRejectsTruncation();
};

void TestRideFileCrc::matchesStandardVectors_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<quint16>("expected");

    QTest::newRow("empty")
        << QByteArray() << quint16(0x0000);
    QTest::newRow("zeroes")
        << QByteArray::fromHex("000000") << quint16(0xc6cc);
    QTest::newRow("binary")
        << QByteArray::fromHex("0faaff") << quint16(0xd1fc);
    QTest::newRow("mixed")
        << QByteArray::fromHex("0a123456") << quint16(0xf62c);
    QTest::newRow("check")
        << QByteArrayLiteral("123456789") << quint16(0x906e);
}

void TestRideFileCrc::matchesStandardVectors()
{
    QFETCH(QByteArray, bytes);
    QFETCH(quint16, expected);
    quint16 checksum = 0xbeef;

    QVERIFY(compute(bytes, checksum));
    QCOMPARE(checksum, expected);
}

void TestRideFileCrc::matchesQtChecksum_data()
{
    QTest::addColumn<QByteArray>("bytes");

    QTest::newRow("empty") << QByteArray();
    QTest::newRow("standard-vector")
        << QByteArrayLiteral("123456789");
    QTest::newRow("binary")
        << QByteArray::fromHex(
               "000102037f80feff102030405060708090");
    QTest::newRow("chunk-boundary-minus-one")
        << QByteArray(
               RideFileCRC::ReadChunkSize - 1, '\x5a');
    QTest::newRow("chunk-boundary")
        << QByteArray(
               RideFileCRC::ReadChunkSize, '\xa5');
    QTest::newRow("multiple-chunks")
        << QByteArray(
               RideFileCRC::ReadChunkSize * 2 + 17, '\x3c');
}

void TestRideFileCrc::matchesQtChecksum()
{
    QFETCH(QByteArray, bytes);
    quint16 checksum = 0xbeef;

    QVERIFY(compute(bytes, checksum));
    QCOMPARE(
        checksum,
        qChecksum(QByteArrayView(bytes)));
}

void TestRideFileCrc::acceptsPartialReads()
{
    const QByteArray bytes(
        RideFileCRC::ReadChunkSize + 17, '\x42');
    ControlledDevice input(bytes);
    input.setMaximumRead(3);
    quint16 checksum = 0xbeef;

    QVERIFY(RideFileCRC::compute(input, checksum));
    QCOMPARE(
        checksum,
        qChecksum(QByteArrayView(bytes)));
}

void TestRideFileCrc::rejectsPrematureEndOfFile()
{
    ControlledDevice input(QByteArrayLiteral("short"));
    input.setReportedSize(10);
    quint16 checksum = 0xbeef;

    QVERIFY(!RideFileCRC::compute(input, checksum));
    QCOMPARE(checksum, quint16(0xbeef));
}

void TestRideFileCrc::rejectsReadFailure()
{
    ControlledDevice input(QByteArrayLiteral("read failure"));
    input.setMaximumRead(3);
    input.setFailOnRead(2);
    QVERIFY(input.seek(2));
    quint16 checksum = 0xbeef;

    QVERIFY(!RideFileCRC::compute(input, checksum));
    QCOMPARE(checksum, quint16(0xbeef));
    QCOMPARE(input.pos(), qint64(2));
}

void TestRideFileCrc::rejectsGrowth()
{
    ControlledDevice input(QByteArrayLiteral("original"));
    input.setAfterRead(
        [](ControlledDevice &device, int readCall) {
            if (readCall == 1) {
                device.append(QByteArrayLiteral("x"));
                device.setReportedSize(9);
            }
        });
    quint16 checksum = 0xbeef;

    QVERIFY(!RideFileCRC::compute(input, checksum));
    QCOMPARE(checksum, quint16(0xbeef));
}

void TestRideFileCrc::rejectsShrink()
{
    ControlledDevice input(QByteArrayLiteral("original"));
    input.setAfterRead(
        [](ControlledDevice &device, int readCall) {
            if (readCall == 1)
                device.setReportedSize(4);
        });
    quint16 checksum = 0xbeef;

    QVERIFY(!RideFileCRC::compute(input, checksum));
    QCOMPARE(checksum, quint16(0xbeef));
}

void TestRideFileCrc::rejectsChangedSnapshot()
{
    bool snapshotCurrent = true;
    ControlledDevice input(QByteArrayLiteral("same size"));
    QVERIFY(input.seek(3));
    input.setAfterRead(
        [&](ControlledDevice &, int readCall) {
            if (readCall == 1)
                snapshotCurrent = false;
        });
    quint16 checksum = 0xbeef;

    QVERIFY(!RideFileCRC::compute(
        input,
        checksum,
        [&]() { return snapshotCurrent; }));
    QCOMPARE(checksum, quint16(0xbeef));
    QCOMPARE(input.pos(), qint64(3));
}

void TestRideFileCrc::emptySourceStillValidatesSnapshot()
{
    QBuffer input;
    input.setData(QByteArray());
    QVERIFY(input.open(QIODevice::ReadOnly));
    int validationCalls = 0;
    quint16 checksum = 0xbeef;

    QVERIFY(!RideFileCRC::compute(
        input,
        checksum,
        [&]() {
            ++validationCalls;
            return false;
        }));
    QCOMPARE(validationCalls, 1);
    QCOMPARE(checksum, quint16(0xbeef));
}

void TestRideFileCrc::boundsEveryReadRequest()
{
    const qint64 logicalSize =
        RideFileCRC::ReadChunkSize * 64 + 17;
    GeneratedZeroDevice input(logicalSize);
    quint16 checksum = 0xbeef;

    QVERIFY(RideFileCRC::compute(input, checksum));
    QCOMPARE(input.totalRead(), logicalSize);
    QVERIFY(input.readCalls() >= 65);
    QVERIFY(input.maximumRequested()
            <= RideFileCRC::ReadChunkSize);
}

void TestRideFileCrc::rejectsOversizedSourceBeforeReading()
{
    GeneratedZeroDevice input(
        RideFileCRC::MaximumSourceSize + 1);
    quint16 checksum = 0xbeef;

    QVERIFY(!RideFileCRC::compute(input, checksum));
    QCOMPARE(input.totalRead(), qint64(0));
    QCOMPARE(checksum, quint16(0xbeef));
}

void TestRideFileCrc::restoresInputPosition()
{
    QBuffer input;
    input.setData(QByteArrayLiteral("0123456789"));
    QVERIFY(input.open(QIODevice::ReadOnly));
    QVERIFY(input.seek(4));
    quint16 checksum = 0xbeef;

    QVERIFY(RideFileCRC::compute(input, checksum));
    QCOMPARE(input.pos(), qint64(4));
}

void TestRideFileCrc::rejectsUnavailableInput()
{
    QBuffer closed;
    closed.setData(QByteArrayLiteral("closed"));
    quint16 checksum = 0xbeef;

    QVERIFY(!RideFileCRC::compute(closed, checksum));
    QCOMPARE(checksum, quint16(0xbeef));

    QBuffer writeOnly;
    QVERIFY(writeOnly.open(QIODevice::WriteOnly));
    QVERIFY(!RideFileCRC::compute(writeOnly, checksum));
    QCOMPARE(checksum, quint16(0xbeef));
}

void TestRideFileCrc::windowsIdentityFallbackPolicy()
{
    RideFileCRC::WindowsIdentityForTest first;
    first.extendedAvailable = true;
    first.legacyVolume = 7;
    first.legacyIndex = 11;
    RideFileCRC::WindowsIdentityForTest same = first;

    QVERIFY(RideFileCRC::windowsIdentityUsableForTest(first));
    QVERIFY(RideFileCRC::windowsIdentitiesMatchForTest(
        first, same));

    same.legacyIndex = 12;
    QVERIFY(!RideFileCRC::windowsIdentitiesMatchForTest(
        first, same));

    first.extendedId[0] = 0x42;
    first.extendedVolume = 101;
    same = first;
    same.legacyIndex = 99;
    QVERIFY(RideFileCRC::windowsIdentitiesMatchForTest(
        first, same));

    same.extendedId[1] = 0x24;
    QVERIFY(!RideFileCRC::windowsIdentitiesMatchForTest(
        first, same));

    RideFileCRC::WindowsIdentityForTest missing;
    QVERIFY(!RideFileCRC::windowsIdentityUsableForTest(
        missing));
}

void TestRideFileCrc::fileSnapshotAcceptsStableFile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("stable.fit"));
    const QByteArray bytes(
        RideFileCRC::ReadChunkSize + 17, '\x37');
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
    file.close();

    quint16 checksum = 0xbeef;
    QVERIFY(RideFileCRC::computeFileForTest(
        path, checksum, {}));
    QCOMPARE(checksum, qChecksum(QByteArrayView(bytes)));
}

void TestRideFileCrc::fileSnapshotRejectsAtomicReplacement()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("replace.fit"));
    const QByteArray original("original payload");
    const QByteArray replacement("replacement data");
    QCOMPARE(original.size(), replacement.size());

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(original), original.size());
    file.close();
    const QDateTime originalModified =
        QFileInfo(path).lastModified();

    QSaveFile staged(path);
    QVERIFY(staged.open(QIODevice::WriteOnly));
    QCOMPARE(staged.write(replacement), replacement.size());
    bool replacementCommitted = false;
    RideFileCRC::FileTestHooks hooks;
    hooks.afterInitialSnapshot = [&]() {
        replacementCommitted = staged.commit();
        QFile current(path);
        if (replacementCommitted
            && current.open(QIODevice::ReadWrite)) {
            replacementCommitted = current.setFileTime(
                originalModified,
                QFileDevice::FileModificationTime);
        } else {
            replacementCommitted = false;
        }
    };
    quint16 checksum = 0xbeef;

    const bool computed =
        RideFileCRC::computeFileForTest(
            path, checksum, hooks);
    if (!replacementCommitted)
        QSKIP("Platform did not permit replacing an open file");
    QVERIFY(!computed);
    QCOMPARE(checksum, quint16(0xbeef));
}

void TestRideFileCrc::fileSnapshotRejectsRestoredMtimeOverwrite()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("overwrite.fit"));
    const QByteArray original("original payload");
    const QByteArray replacement("replacement data");
    QCOMPARE(original.size(), replacement.size());

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(original), original.size());
    file.close();
#if defined(Q_OS_UNIX)
    struct stat originalStatus {};
    QVERIFY(::stat(
                QFile::encodeName(path).constData(),
                &originalStatus)
            == 0);
#else
    const QDateTime originalModified =
        QFileInfo(path).lastModified();
#endif

    bool overwriteCompleted = false;
    bool exactMtimeRestored = false;
    bool metadataChangeObserved = false;
    RideFileCRC::FileTestHooks hooks;
    hooks.afterInitialSnapshot = [&]() {
        QTest::qSleep(2);
        QFile current(path);
        overwriteCompleted =
            current.open(
                QIODevice::WriteOnly
                | QIODevice::Truncate)
            && current.write(replacement)
                == replacement.size();
        current.close();
#if defined(Q_OS_UNIX)
        if (overwriteCompleted) {
            struct timespec times[2] {};
#if defined(Q_OS_MACOS)
            times[0] = originalStatus.st_atimespec;
            times[1] = originalStatus.st_mtimespec;
#else
            times[0] = originalStatus.st_atim;
            times[1] = originalStatus.st_mtim;
#endif
            exactMtimeRestored =
                ::utimensat(
                    AT_FDCWD,
                    QFile::encodeName(path).constData(),
                    times,
                    0)
                == 0;
            struct stat finalStatus {};
            if (exactMtimeRestored
                && ::stat(
                       QFile::encodeName(path).constData(),
                       &finalStatus)
                    == 0) {
#if defined(Q_OS_MACOS)
                exactMtimeRestored =
                    finalStatus.st_mtimespec.tv_sec
                        == originalStatus.st_mtimespec.tv_sec
                    && finalStatus.st_mtimespec.tv_nsec
                        == originalStatus.st_mtimespec.tv_nsec;
                metadataChangeObserved =
                    finalStatus.st_ctimespec.tv_sec
                            != originalStatus.st_ctimespec.tv_sec
                    || finalStatus.st_ctimespec.tv_nsec
                            != originalStatus.st_ctimespec.tv_nsec;
#else
                exactMtimeRestored =
                    finalStatus.st_mtim.tv_sec
                        == originalStatus.st_mtim.tv_sec
                    && finalStatus.st_mtim.tv_nsec
                        == originalStatus.st_mtim.tv_nsec;
                metadataChangeObserved =
                    finalStatus.st_ctim.tv_sec
                            != originalStatus.st_ctim.tv_sec
                    || finalStatus.st_ctim.tv_nsec
                            != originalStatus.st_ctim.tv_nsec;
#endif
            }
        }
#else
        if (overwriteCompleted) {
            QFile restored(path);
            exactMtimeRestored =
                restored.open(QIODevice::ReadWrite)
                && restored.setFileTime(
                originalModified,
                QFileDevice::FileModificationTime);
            metadataChangeObserved = true;
        }
#endif
    };
    quint16 checksum = 0xbeef;

    const bool computed =
        RideFileCRC::computeFileForTest(
            path, checksum, hooks);
    if (!overwriteCompleted)
        QSKIP("Platform did not permit overwriting an open file");
    if (!exactMtimeRestored)
        QSKIP("Filesystem did not permit exact mtime restoration");
    if (!metadataChangeObserved)
        QSKIP("Filesystem did not expose a changed ctime");
    QVERIFY(!computed);
    QCOMPARE(checksum, quint16(0xbeef));
}

void TestRideFileCrc::fileSnapshotRejectsRemoval()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("removed.fit"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(
        file.write(QByteArrayLiteral("activity")),
        qint64(8));
    file.close();

    bool removed = false;
    RideFileCRC::FileTestHooks hooks;
    hooks.afterInitialSnapshot = [&]() {
        removed = QFile::remove(path);
    };
    quint16 checksum = 0xbeef;

    const bool computed =
        RideFileCRC::computeFileForTest(
            path, checksum, hooks);
    if (!removed)
        QSKIP("Platform did not permit removing an open file");
    QVERIFY(!computed);
    QCOMPARE(checksum, quint16(0xbeef));
}

void TestRideFileCrc::fileSnapshotRejectsGrowthAfterFirstChunk()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("grow.fit"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray bytes(
        RideFileCRC::ReadChunkSize * 2 + 17, '\x51');
    QCOMPARE(file.write(bytes), bytes.size());
    file.close();

    bool grew = false;
    RideFileCRC::FileTestHooks hooks;
    hooks.afterChunk = [&](qint64 totalRead) {
        if (!grew
            && totalRead >= RideFileCRC::ReadChunkSize) {
            QFile current(path);
            grew = current.open(QIODevice::Append)
                && current.write("x", 1) == 1;
        }
    };
    quint16 checksum = 0xbeef;

    const bool computed =
        RideFileCRC::computeFileForTest(
            path, checksum, hooks);
    if (!grew)
        QSKIP("Platform did not permit growing an open file");
    QVERIFY(!computed);
    QCOMPARE(checksum, quint16(0xbeef));
}

void TestRideFileCrc::fileSnapshotRejectsShrinkAfterFirstChunk()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("shrink.fit"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray bytes(
        RideFileCRC::ReadChunkSize * 2 + 17, '\x62');
    QCOMPARE(file.write(bytes), bytes.size());
    file.close();

    bool shrank = false;
    RideFileCRC::FileTestHooks hooks;
    hooks.afterChunk = [&](qint64 totalRead) {
        if (!shrank
            && totalRead >= RideFileCRC::ReadChunkSize) {
            QFile current(path);
            shrank = current.open(QIODevice::ReadWrite)
                && current.resize(
                    RideFileCRC::ReadChunkSize + 1);
        }
    };
    quint16 checksum = 0xbeef;

    const bool computed =
        RideFileCRC::computeFileForTest(
            path, checksum, hooks);
    if (!shrank)
        QSKIP("Platform did not permit shrinking an open file");
    QVERIFY(!computed);
    QCOMPARE(checksum, quint16(0xbeef));
}

void TestRideFileCrc::fileSnapshotRejectsNonRegularInput()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    quint16 checksum = 0xbeef;

    QVERIFY(!RideFileCRC::computeFileForTest(
        directory.path(), checksum, {}));
    QCOMPARE(checksum, quint16(0xbeef));
}

void TestRideFileCrc::fileSnapshotRejectsFifoWithoutBlocking()
{
#if defined(Q_OS_UNIX)
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("activity.fifo"));
    const QByteArray nativePath = QFile::encodeName(path);
    QCOMPARE(::mkfifo(nativePath.constData(), 0600), 0);

    const pid_t child = ::fork();
    QVERIFY(child >= 0);
    if (child == 0) {
        ::alarm(2);
        quint16 checksum = 0xbeef;
        const bool computed =
            RideFileCRC::computeFile(path, checksum);
        ::_exit(
            !computed && checksum == quint16(0xbeef)
            ? EXIT_SUCCESS
            : EXIT_FAILURE);
    }

    int status = 0;
    QCOMPARE(::waitpid(child, &status, 0), child);
    QVERIFY2(
        WIFEXITED(status),
        "FIFO source blocked until the child alarm fired");
    QCOMPARE(WEXITSTATUS(status), EXIT_SUCCESS);
#else
    QSKIP("FIFO behavior is Unix-specific");
#endif
}

void TestRideFileCrc::fingerprintFailureLeavesOutputUnchanged()
{
    RideFileCRC::ContentFingerprint original;
    original.byteSize = 7;
    original.sha256 = QByteArray(
        RideFileCRC::Sha256Size, '\x5a');
    original.legacyCrc16 = 0x1234;
    QVERIFY(original.isValid());

    QBuffer closed;
    closed.setData(QByteArrayLiteral("closed"));
    RideFileCRC::ContentFingerprint fingerprint = original;

    QVERIFY(!RideFileCRC::computeFingerprint(
        closed, fingerprint));
    QVERIFY(fingerprint == original);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString missing =
        directory.filePath(QStringLiteral("missing.fit"));
    QVERIFY(!RideFileCRC::computeFileFingerprint(
        missing, fingerprint));
    QVERIFY(fingerprint == original);
}

void TestRideFileCrc::captureProvidesStableStagedBytes()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("stable.fit"));
    const QByteArray original(
        RideFileCRC::ReadChunkSize + 17, '\x6b');
    QFile source(path);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(original), original.size());
    source.close();

    RideFileCRC::StagedSource captured;
    QVERIFY(RideFileCRC::captureFile(path, captured));
    QVERIFY(captured.isValid());
    QFile *staged = captured.fileForReading();
    QVERIFY(staged);
    QVERIFY(!staged->isOpen());
    QVERIFY(staged->fileName() != path);
    QCOMPARE(
        QFileInfo(staged->fileName()).fileName(),
        QFileInfo(path).fileName());

    QVERIFY(source.open(
        QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray replacement(original.size(), '\x19');
    QCOMPARE(
        source.write(replacement),
        replacement.size());
    source.close();

    QVERIFY(staged->open(QIODevice::ReadOnly));
    QCOMPARE(staged->readAll(), original);
    staged->close();

    const RideFileCRC::ContentFingerprint &fingerprint =
        captured.fingerprint();
    QCOMPARE(
        fingerprint.byteSize,
        static_cast<qint64>(original.size()));
    QCOMPARE(
        fingerprint.sha256,
        QCryptographicHash::hash(
            original, QCryptographicHash::Sha256));
    QCOMPARE(
        fingerprint.legacyCrc16,
        qChecksum(QByteArrayView(original)));
}

void TestRideFileCrc::captureFailureLeavesOutputUnchanged()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("valid.fit"));
    QFile source(path);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(
        source.write(QByteArrayLiteral("activity")),
        qint64(8));
    source.close();

    RideFileCRC::StagedSource captured;
    QVERIFY(RideFileCRC::captureFile(path, captured));
    QFile *const originalFile =
        captured.fileForReading();
    QVERIFY(originalFile);
    const QString originalStagedPath =
        originalFile->fileName();
    const RideFileCRC::ContentFingerprint originalFingerprint =
        captured.fingerprint();

    const QString missing =
        directory.filePath(QStringLiteral("missing.fit"));
    QVERIFY(!RideFileCRC::captureFile(missing, captured));
    QVERIFY(captured.isValid());
    QCOMPARE(captured.fileForReading(), originalFile);
    QCOMPARE(
        captured.fileForReading()->fileName(),
        originalStagedPath);
    QVERIFY(
        captured.fingerprint()
        == originalFingerprint);
    QVERIFY(QFileInfo::exists(originalStagedPath));

    RideFileCRC::StagedSource invalid;
    QVERIFY(!RideFileCRC::captureFile(missing, invalid));
    QVERIFY(!invalid.isValid());
    QVERIFY(!invalid.fileForReading());
    QVERIFY(!invalid.fingerprint().isValid());
}

void TestRideFileCrc::fingerprintDistinguishesSameSizeContent()
{
    const QByteArray first =
        QByteArrayLiteral("source-alpha");
    const QByteArray second =
        QByteArrayLiteral("source-omega");
    QCOMPARE(first.size(), second.size());

    RideFileCRC::ContentFingerprint firstFingerprint;
    RideFileCRC::ContentFingerprint secondFingerprint;
    QVERIFY(computeFingerprint(first, firstFingerprint));
    QVERIFY(computeFingerprint(second, secondFingerprint));

    QCOMPARE(
        firstFingerprint.byteSize,
        secondFingerprint.byteSize);
    QVERIFY(
        firstFingerprint.sha256
        != secondFingerprint.sha256);
    QVERIFY(firstFingerprint != secondFingerprint);
}

void TestRideFileCrc::fingerprintDistinguishesCrc16Collision()
{
    std::vector<int> firstSeen(1 << 16, -1);
    QByteArray first;
    QByteArray second;
    for (quint32 value = 0; value <= (1U << 16); ++value) {
        const QByteArray candidate = bytesForValue(value);
        const quint16 checksum =
            qChecksum(QByteArrayView(candidate));
        if (firstSeen[checksum] >= 0) {
            first = bytesForValue(
                static_cast<quint32>(
                    firstSeen[checksum]));
            second = candidate;
            break;
        }
        firstSeen[checksum] =
            static_cast<int>(value);
    }

    QVERIFY(!first.isEmpty());
    QVERIFY(first != second);
    QCOMPARE(first.size(), second.size());

    RideFileCRC::ContentFingerprint firstFingerprint;
    RideFileCRC::ContentFingerprint secondFingerprint;
    QVERIFY(computeFingerprint(first, firstFingerprint));
    QVERIFY(computeFingerprint(second, secondFingerprint));
    QCOMPARE(
        firstFingerprint.legacyCrc16,
        secondFingerprint.legacyCrc16);
    QCOMPARE(
        firstFingerprint.byteSize,
        secondFingerprint.byteSize);
    QVERIFY(
        firstFingerprint.sha256
        != secondFingerprint.sha256);
    QVERIFY(firstFingerprint != secondFingerprint);
}

void TestRideFileCrc::captureRejectsSourceRewriteAfterCopy()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("rewrite.fit"));
    const QByteArray original(
        RideFileCRC::ReadChunkSize + 17, '\x21');
    const QByteArray replacement(
        original.size(), '\x72');
    QFile source(path);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(original), original.size());
    source.close();

    RideFileCRC::StagedSource captured;
    QVERIFY(RideFileCRC::captureFile(path, captured));
    QFile *const originalStagedFile =
        captured.fileForReading();
    QVERIFY(originalStagedFile);
    const QString originalStagedPath =
        originalStagedFile->fileName();
    const RideFileCRC::ContentFingerprint originalFingerprint =
        captured.fingerprint();

    bool rewriteCompleted = false;
    RideFileCRC::FileTestHooks hooks;
    hooks.afterSourceCopied = [&]() {
        QFile current(path);
        rewriteCompleted =
            current.open(
                QIODevice::WriteOnly
                | QIODevice::Truncate)
            && current.write(replacement)
                == replacement.size();
        current.close();
    };

    const bool captureSucceeded =
        RideFileCRC::captureFileForTest(
            path, captured, hooks);

    QVERIFY(rewriteCompleted);
    QVERIFY(!captureSucceeded);
    QCOMPARE(
        captured.fileForReading(),
        originalStagedFile);
    QCOMPARE(
        captured.fileForReading()->fileName(),
        originalStagedPath);
    QVERIFY(
        captured.fingerprint()
        == originalFingerprint);
    QVERIFY(QFileInfo::exists(originalStagedPath));
}

void TestRideFileCrc::captureRejectsAtomicReplacement()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("replace.fit"));
    const QByteArray original(
        RideFileCRC::ReadChunkSize * 2 + 17, '\x33');
    const QByteArray replacement(
        original.size(), '\x44');
    QFile source(path);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(original), original.size());
    source.close();

    QSaveFile stagedReplacement(path);
    QVERIFY(stagedReplacement.open(QIODevice::WriteOnly));
    QCOMPARE(
        stagedReplacement.write(replacement),
        replacement.size());
    bool replacementAttempted = false;
    bool replacementCommitted = false;
    RideFileCRC::FileTestHooks hooks;
    hooks.afterChunk = [&](qint64 totalRead) {
        if (!replacementAttempted
            && totalRead >= RideFileCRC::ReadChunkSize) {
            replacementAttempted = true;
            replacementCommitted =
                stagedReplacement.commit();
        }
    };
    RideFileCRC::StagedSource captured;

    const bool captureSucceeded =
        RideFileCRC::captureFileForTest(
            path, captured, hooks);
    QVERIFY(replacementAttempted);
    if (!replacementCommitted)
        QSKIP("Platform did not permit replacing an open file");
    QVERIFY(!captureSucceeded);
    QVERIFY(!captured.isValid());
}

void TestRideFileCrc::captureRejectsTruncation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("truncate.fit"));
    const QByteArray original(
        RideFileCRC::ReadChunkSize * 2 + 17, '\x55');
    QFile source(path);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(original), original.size());
    source.close();

    bool truncationAttempted = false;
    bool truncated = false;
    RideFileCRC::FileTestHooks hooks;
    hooks.afterChunk = [&](qint64 totalRead) {
        if (!truncationAttempted
            && totalRead >= RideFileCRC::ReadChunkSize) {
            truncationAttempted = true;
            QFile current(path);
            truncated =
                current.open(QIODevice::ReadWrite)
                && current.resize(
                    RideFileCRC::ReadChunkSize + 1);
        }
    };
    RideFileCRC::StagedSource captured;

    const bool captureSucceeded =
        RideFileCRC::captureFileForTest(
            path, captured, hooks);
    QVERIFY(truncationAttempted);
    if (!truncated)
        QSKIP("Platform did not permit truncating an open file");
    QVERIFY(!captureSucceeded);
    QVERIFY(!captured.isValid());
}

QTEST_GUILESS_MAIN(TestRideFileCrc)

#include "testRideFileCrc.moc"
