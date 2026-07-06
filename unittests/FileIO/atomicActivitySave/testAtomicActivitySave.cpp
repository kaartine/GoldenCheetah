#include <QtTest>

#include "AtomicFileWriter.h"
#include "JsonRideFile.h"
#include "RideCache.h"
#include "SaveDialogs.h"

#include <QFile>
#include <QTemporaryDir>

void resetAtomicActivitySaveProcessorStub();
void setAtomicActivitySaveProcessorFailure(bool enabled);
int atomicActivitySaveProcessorCalls();

namespace {

enum class FailurePoint {
    None,
    Open,
    ShortWrite,
    Flush,
    Commit,
    CorruptCommit,
    MissingCommit
};

class FaultInjectingWriter final : public AtomicFileWriter
{
public:
    FaultInjectingWriter(
        const QString &path, FailurePoint failure,
        std::function<void()> afterCommit = std::function<void()>())
        : path_(path), failure_(failure),
          afterCommit_(std::move(afterCommit))
    {
    }

    bool open() override
    {
        return failure_ != FailurePoint::Open;
    }

    qint64 write(const QByteArray &data) override
    {
        staged_ = data;
        if (failure_ == FailurePoint::ShortWrite) {
            return data.isEmpty() ? -1 : data.size() - 1;
        }
        return data.size();
    }

    bool flush() override
    {
        return failure_ != FailurePoint::Flush;
    }

    bool commit() override
    {
        if (failure_ == FailurePoint::Commit) {
            return false;
        }
        if (failure_ == FailurePoint::MissingCommit) {
            QFile::remove(path_);
            return true;
        }

        QFile target(path_);
        if (!target.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        QByteArray committed = staged_;
        if (failure_ == FailurePoint::CorruptCommit) {
            committed.append("corrupt");
        }
        const bool success =
            target.write(committed) == committed.size()
            && target.flush();
        if (success && afterCommit_) {
            afterCommit_();
        }
        return success;
    }

    void cancelWriting() override
    {
        staged_.clear();
    }

    QString errorString() const override
    {
        return QStringLiteral("injected failure");
    }

private:
    QString path_;
    FailurePoint failure_;
    QByteArray staged_;
    std::function<void()> afterCommit_;
};

class ObservableRideFile final : public RideFile
{
public:
    using RideFile::RideFile;
    void markSaved() { emitSaved(); }
};

class TestSaveSingleDialog final : public SaveSingleDialogWidget
{
public:
    explicit TestSaveSingleDialog(RideItem *rideItem)
        : SaveSingleDialogWidget(nullptr, nullptr, rideItem)
    {
    }

    bool saveResult = true;
    int saveCalls = 0;
    int errorReports = 0;

protected:
    bool saveRide(QString &error) override
    {
        ++saveCalls;
        if (!saveResult) {
            error = QStringLiteral("injected dialog save failure");
        }
        return saveResult;
    }

    void reportSaveError(const QString &) override
    {
        ++errorReports;
    }
};

class TestSaveOnExitDialog final : public SaveOnExitDialogWidget
{
public:
    explicit TestSaveOnExitDialog(const QList<RideItem *> &rides)
        : SaveOnExitDialogWidget(nullptr, nullptr, rides)
    {
    }

    QHash<RideItem *, bool> results;
    QList<RideItem *> calls;

protected:
    bool saveRide(RideItem *rideItem) override
    {
        calls.append(rideItem);
        const bool result = results.value(rideItem, true);
        if (result) {
            rideItem->setDirty(false);
        }
        return result;
    }
};

QString activityFileName(const QDateTime &startTime, const QString &suffix)
{
    return startTime.toString(QStringLiteral("yyyy_MM_dd_hh_mm_ss"))
        + QLatin1Char('.') + suffix;
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

void writeFixture(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
             qPrintable(file.errorString()));
    QCOMPARE(file.write(bytes), static_cast<qint64>(bytes.size()));
    QVERIFY(file.flush());
}

} // namespace

class TestAtomicActivitySave : public QObject
{
    Q_OBJECT

private slots:
    void cleanup();
    void failurePreservesOriginalAndDirty_data();
    void failurePreservesOriginalAndDirty();
    void successReplacesOriginalAndMarksClean();
    void finalizeFailureKeepsDirty();
    void qSaveFileWriterCommitsReplacement();
    void conversionMovesOriginalToBackup();
    void jsonRenameRemovesSupersededSource();
    void finalizationFailurePreservesTargetAndKeepsDirty();
    void finalizationFailurePreservesPreviousBackup();
    void finalizationSyncFailureRestoresSource_data();
    void finalizationSyncFailureRestoresSource();
    void atomicWriterRejectsSymlinkTarget();
    void atomicWriterRejectsCollisionAtCommit();
    void atomicWriterRejectsLockedTarget();
    void newWriterRollsBackPartialPublishFailure();
    void stagedFileSetPublishesAll();
    void publicationFailureSkipsCacheUpdate();
    void publicationSuccessUpdatesCacheAfterPublish();
    void stagedFileSetRollsBackOnMiddleFailure();
    void stagedFileSetRollsBackPartiallyPublishedFailure();
    void stagedFileSetCleansStagingOnCollision();
    void stagedFileSetRejectsUnsafePathGraphs();
    void stagedFileSetRejectsInvalidAndDuplicateStages();
    void stagedFileSetReleasesLocksAfterLockFailure();
    void stagedFileSetFinalizesWhileTargetsAreLocked();
    void stagedFileSetReturnsSuccessfulFinalizerWarning();
    void stagedFileSetRollsBackWhenFinalizerFails();
    void lockSetKeepsCaseDistinctPaths();
    void atomicWriterPreservesConcurrentNewTarget();
    void jsonWriterFailurePreservesOriginal_data();
    void jsonWriterFailurePreservesOriginal();
    void jsonWriterKeepsUtf8BomAndRoundTrips();
    void jsonReaderRejectsMalformedPayload();
    void jsonWriterRejectsOpenTarget();
    void jsonWriterRejectsExistingTargetWhenReplacementDisabled();
    void jsonReaderKeepsLegacyLatin1Compatibility();
    void jsonReaderPreservesUtf8ReplacementCharacter();
    void jsonReaderPreservesLargeInteger();
    void jsonReaderPreservesPositiveExponent();
    void jsonRoundTripsReferenceAndSampleState();
    void saveHelpersRejectInvalidOperations();
    void successfulTransactionPreservesLiveObjectState();
    void transactionPreservesZeroValuedSeriesPresence();
    void processorFailureSkipsWriteAndKeepsDirty();
    void failedSaveRestoresHistoryUntilCommit();
    void candidateSaveTransfersOnlyAfterSuccess();
    void defaultProcessorFailurePreservesFileAndDirtyState();
    void mainWindowConvertsSourceAfterCommit();
    void mainWindowRenamesJsonAfterCommit();
    void mainWindowRejectsTargetCollision();
    void mainWindowHoldsSourceAndTargetLocks();
    void mainWindowFinalizeFailureRemainsRetryable();
    void mainWindowRejectsSourceChangedDuringSave();
    void mainWindowSaveSilentPropagatesFailure();
    void mainWindowSaveSilentPreservesUppercaseJsonPath();
    void mainWindowSaveRideSingleDialogPropagatesResult();
    void saveSingleDialogSaveAndAbandon();
    void saveOnExitDialogStopsUntilAllSelectedSave();
    void saveOnExitDialogDefersSkippedStateUntilSuccess();
    void rideCacheSaveActivityPropagatesFailure();
    void rideCacheSaveActivitiesAggregatesFailures();
};

void TestAtomicActivitySave::cleanup()
{
    resetAtomicActivitySaveProcessorStub();
}

void TestAtomicActivitySave::failurePreservesOriginalAndDirty_data()
{
    QTest::addColumn<int>("failurePoint");
    QTest::addColumn<QString>("expectedError");

    QTest::newRow("open")
        << static_cast<int>(FailurePoint::Open) << QStringLiteral("open");
    QTest::newRow("short-write")
        << static_cast<int>(FailurePoint::ShortWrite) << QStringLiteral("write");
    QTest::newRow("flush")
        << static_cast<int>(FailurePoint::Flush) << QStringLiteral("flush");
    QTest::newRow("commit")
        << static_cast<int>(FailurePoint::Commit) << QStringLiteral("commit");
    QTest::newRow("corrupt-commit")
        << static_cast<int>(FailurePoint::CorruptCommit) << QStringLiteral("match");
    QTest::newRow("missing-commit")
        << static_cast<int>(FailurePoint::MissingCommit) << QStringLiteral("verify");
}

void TestAtomicActivitySave::failurePreservesOriginalAndDirty()
{
    QFETCH(int, failurePoint);
    const FailurePoint failure = static_cast<FailurePoint>(failurePoint);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("original activity bytes");
    const QByteArray replacement("replacement activity bytes");
    writeFixture(path, original);

    bool rideFileDirty = true;
    bool rideItemDirty = true;
    QString error;
    const AtomicFileWriterFactory factory = [failure](const QString &target, AtomicFileMode) {
        return std::unique_ptr<AtomicFileWriter>(new FaultInjectingWriter(target, failure));
    };

    const bool saved = completeActivitySave(
        [&](QString &stepError) {
            return writeFileAtomically(path, replacement, factory, stepError);
        },
        [](QString &) { return true; },
        [&]() {
            rideFileDirty = false;
            rideItemDirty = false;
        },
        error);

    QVERIFY(!saved);
    QVERIFY(!error.isEmpty());
    QCOMPARE(readAll(path), original);
    QVERIFY(rideFileDirty);
    QVERIFY(rideItemDirty);
}

void TestAtomicActivitySave::successReplacesOriginalAndMarksClean()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("original activity bytes");
    const QByteArray replacement("replacement activity bytes");
    writeFixture(path, original);

    bool rideFileDirty = true;
    bool rideItemDirty = true;
    QString error;
    const AtomicFileWriterFactory factory = [](const QString &target, AtomicFileMode) {
        return std::unique_ptr<AtomicFileWriter>(
            new FaultInjectingWriter(target, FailurePoint::None));
    };

    QVERIFY(completeActivitySave(
        [&](QString &stepError) {
            return writeFileAtomically(path, replacement, factory, stepError);
        },
        [](QString &) { return true; },
        [&]() {
            rideFileDirty = false;
            rideItemDirty = false;
        },
        error));

    QVERIFY(error.isEmpty());
    QCOMPARE(readAll(path), replacement);
    QVERIFY(!rideFileDirty);
    QVERIFY(!rideItemDirty);
}

void TestAtomicActivitySave::finalizeFailureKeepsDirty()
{
    bool rideFileDirty = true;
    bool rideItemDirty = true;
    QString error;

    const bool saved = completeActivitySave(
        [](QString &) { return true; },
        [](QString &stepError) {
            stepError = QStringLiteral("injected rename failure");
            return false;
        },
        [&]() {
            rideFileDirty = false;
            rideItemDirty = false;
        },
        error);

    QVERIFY(!saved);
    QCOMPARE(error, QStringLiteral("injected rename failure"));
    QVERIFY(rideFileDirty);
    QVERIFY(rideItemDirty);
}

void TestAtomicActivitySave::qSaveFileWriterCommitsReplacement()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("original activity bytes");
    const QByteArray replacement("replacement activity bytes");
    writeFixture(path, original);

    QString error;
    QVERIFY(writeFileAtomically(path, replacement,
                                qSaveFileWriterFactory(), error));
    QVERIFY(error.isEmpty());
    QCOMPARE(readAll(path), replacement);
}

void TestAtomicActivitySave::conversionMovesOriginalToBackup()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sourcePath = dir.filePath(QStringLiteral("activity.fit"));
    const QString targetPath = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("original imported activity");
    const QByteArray replacement("committed json activity");
    writeFixture(sourcePath, original);
    writeFixture(targetPath, replacement);

    bool rideFileDirty = true;
    bool rideItemDirty = true;
    QString error;
    QVERIFY(completeActivitySave(
        [](QString &) { return true; },
        [&](QString &stepError) {
            return finalizeActivityFileReplacement(sourcePath, targetPath,
                                                   true, stepError);
        },
        [&]() {
            rideFileDirty = false;
            rideItemDirty = false;
        },
        error));

    QVERIFY(error.isEmpty());
    QVERIFY(!QFile::exists(sourcePath));
    QCOMPARE(readAll(sourcePath + QStringLiteral(".bak")), original);
    QCOMPARE(readAll(targetPath), replacement);
    QVERIFY(!rideFileDirty);
    QVERIFY(!rideItemDirty);
}

void TestAtomicActivitySave::jsonRenameRemovesSupersededSource()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sourcePath = dir.filePath(QStringLiteral("old.json"));
    const QString targetPath = dir.filePath(QStringLiteral("new.json"));
    writeFixture(sourcePath, QByteArray("old json"));
    writeFixture(targetPath, QByteArray("new json"));

    QString error;
    QVERIFY(finalizeActivityFileReplacement(sourcePath, targetPath,
                                            false, error));
    QVERIFY(error.isEmpty());
    QVERIFY(!QFile::exists(sourcePath));
    QCOMPARE(readAll(targetPath), QByteArray("new json"));
}

void TestAtomicActivitySave::finalizationFailurePreservesTargetAndKeepsDirty()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString missingSource = dir.filePath(QStringLiteral("missing.fit"));
    const QString targetPath = dir.filePath(QStringLiteral("activity.json"));
    writeFixture(targetPath, QByteArray("committed json activity"));

    bool rideFileDirty = true;
    bool rideItemDirty = true;
    QString error;
    const bool saved = completeActivitySave(
        [](QString &) { return true; },
        [&](QString &stepError) {
            return finalizeActivityFileReplacement(missingSource, targetPath,
                                                   true, stepError);
        },
        [&]() {
            rideFileDirty = false;
            rideItemDirty = false;
        },
        error);

    QVERIFY(!saved);
    QVERIFY(!error.isEmpty());
    QCOMPARE(readAll(targetPath), QByteArray("committed json activity"));
    QVERIFY(rideFileDirty);
    QVERIFY(rideItemDirty);
}

void TestAtomicActivitySave::finalizationFailurePreservesPreviousBackup()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString missingSource = dir.filePath(QStringLiteral("missing.fit"));
    const QString backupPath = missingSource + QStringLiteral(".bak");
    const QString targetPath = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray previousBackup("previous backup");
    writeFixture(backupPath, previousBackup);
    writeFixture(targetPath, QByteArray("committed json activity"));

    QString error;
    QVERIFY(!finalizeActivityFileReplacement(missingSource, targetPath,
                                             true, error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readAll(backupPath), previousBackup);
    QCOMPARE(readAll(targetPath), QByteArray("committed json activity"));
}

void TestAtomicActivitySave::finalizationSyncFailureRestoresSource_data()
{
    QTest::addColumn<bool>("keepSourceBackup");
    QTest::newRow("remove-source") << false;
    QTest::newRow("keep-backup") << true;
}

void TestAtomicActivitySave::finalizationSyncFailureRestoresSource()
{
    QFETCH(bool, keepSourceBackup);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sourcePath =
        dir.filePath(QStringLiteral("original.fit"));
    const QString targetPath =
        dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("original activity");
    const QByteArray replacement("replacement activity");
    const QByteArray previousBackup("previous backup");
    writeFixture(sourcePath, original);
    writeFixture(targetPath, replacement);
    if (keepSourceBackup) {
        writeFixture(sourcePath + QStringLiteral(".bak"), previousBackup);
    }

    int syncCalls = 0;
    const auto failDirectorySync =
        [&syncCalls](const QString &, QString &syncError) {
            ++syncCalls;
            syncError = QStringLiteral(
                "injected directory sync failure");
            return false;
        };

    QString error;
    QVERIFY(!finalizeActivityFileReplacement(
        sourcePath, targetPath, keepSourceBackup, error,
        failDirectorySync));
    QVERIFY(!error.isEmpty());
    QVERIFY(syncCalls >= 1);
    QCOMPARE(readAll(sourcePath), original);
    QCOMPARE(readAll(targetPath), replacement);
    if (keepSourceBackup) {
        QCOMPARE(readAll(sourcePath + QStringLiteral(".bak")),
                 previousBackup);
    } else {
        QVERIFY(!QFile::exists(sourcePath + QStringLiteral(".bak")));
    }
}

void TestAtomicActivitySave::atomicWriterRejectsSymlinkTarget()
{
#ifdef Q_OS_WIN
    QSKIP("QFile::link creates shortcuts rather than symlinks on Windows");
#endif
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString referentPath =
        dir.filePath(QStringLiteral("referent.json"));
    const QString linkPath =
        dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("referent bytes");
    writeFixture(referentPath, original);
    QVERIFY(QFile::link(referentPath, linkPath));
    QVERIFY(QFileInfo(linkPath).isSymLink());

    QString error;
    QVERIFY(!writeFileAtomically(
        linkPath, QByteArray("replacement"),
        qSaveFileWriterFactory(), error));
    QVERIFY(error.contains(QStringLiteral("symbolic"),
                           Qt::CaseInsensitive));
    QCOMPARE(readAll(referentPath), original);
    QVERIFY(QFileInfo(linkPath).isSymLink());
}

void TestAtomicActivitySave::atomicWriterRejectsCollisionAtCommit()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray replacement("replacement activity");
    const QByteArray concurrent("concurrent activity");

    NewAtomicFileWriter writer(path);
    QVERIFY(writer.open());
    QCOMPARE(writer.write(replacement),
             static_cast<qint64>(replacement.size()));
    QVERIFY(writer.flush());
    writeFixture(path, concurrent);

    QVERIFY(!writer.commit());
    QVERIFY(writer.errorString().contains(
        QStringLiteral("concurrently"), Qt::CaseInsensitive));
    QCOMPARE(readAll(path), concurrent);
}

void TestAtomicActivitySave::atomicWriterRejectsLockedTarget()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("original activity");
    writeFixture(path, original);

    QLockFile lock(dir.filePath(QStringLiteral(".activity.json.lock")));
    lock.setStaleLockTime(0);
    QVERIFY(lock.tryLock(0));

    int factoryCalls = 0;
    const AtomicFileWriterFactory factory =
        [&](const QString &target, AtomicFileMode mode) {
            ++factoryCalls;
            return qSaveFileWriterFactory()(target, mode);
        };
    QString error;
    QVERIFY(!writeFileAtomically(
        path, QByteArray("replacement"), factory, error));
    QCOMPARE(factoryCalls, 0);
    QVERIFY(error.contains(QStringLiteral("already being saved"),
                           Qt::CaseInsensitive));
    QCOMPARE(readAll(path), original);
}

void TestAtomicActivitySave::newWriterRollsBackPartialPublishFailure()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    const AtomicPublishFunction partialPublish =
        [](const QString &, const QString &target,
           bool &temporaryMoved, QString &error) {
            QFile published(target);
            if (!published.open(QIODevice::WriteOnly)
                || published.write("partial") != 7
                || !published.flush()) {
                error = published.errorString();
                return false;
            }
            published.close();
            temporaryMoved = true;
            error = QStringLiteral("injected post-publication failure");
            return false;
        };

    NewAtomicFileWriter writer(path, partialPublish);
    QVERIFY(writer.open());
    QCOMPARE(writer.write(QByteArray("complete activity")),
             qint64(17));
    QVERIFY(writer.flush());
    QVERIFY(!writer.commit());
    QVERIFY(writer.errorString().contains(
        QStringLiteral("post-publication")));
    QVERIFY(!QFile::exists(path));
    QVERIFY(QDir(dir.path()).entryList(
        QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}

void TestAtomicActivitySave::stagedFileSetPublishesAll()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString firstStage = dir.filePath(QStringLiteral(".first.stage"));
    const QString secondStage = dir.filePath(QStringLiteral(".second.stage"));
    const QString firstTarget = dir.filePath(QStringLiteral("first.json"));
    const QString secondTarget = dir.filePath(QStringLiteral("second.json"));
    writeFixture(firstStage, QByteArray("first activity"));
    writeFixture(secondStage, QByteArray("second activity"));

    QString error;
    QVERIFY2(publishStagedFileSet(
                 {
                     StagedFilePublication(firstStage, firstTarget),
                     StagedFilePublication(secondStage, secondTarget)
                 },
                 error),
             qPrintable(error));
    QVERIFY(error.isEmpty());
    QVERIFY(!QFile::exists(firstStage));
    QVERIFY(!QFile::exists(secondStage));
    QCOMPARE(readAll(firstTarget), QByteArray("first activity"));
    QCOMPARE(readAll(secondTarget), QByteArray("second activity"));
}

void TestAtomicActivitySave::publicationFailureSkipsCacheUpdate()
{
    bool cacheUpdated = false;
    QString error;

    QVERIFY(!publishActivityBeforeCacheUpdate(
        [&](QString &publishError) {
            publishError =
                QStringLiteral("injected activity publication failure");
            return false;
        },
        [&]() { cacheUpdated = true; },
        error));

    QVERIFY(!cacheUpdated);
    QCOMPARE(
        error,
        QStringLiteral("injected activity publication failure"));
}

void TestAtomicActivitySave::
publicationSuccessUpdatesCacheAfterPublish()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString stage =
        dir.filePath(QStringLiteral(".activity.stage"));
    const QString target =
        dir.filePath(QStringLiteral("activity.json"));
    const QByteArray contents("durable activity");
    writeFixture(stage, contents);

    bool cacheUpdated = false;
    QString error;
    QVERIFY2(publishActivityBeforeCacheUpdate(
                 [&](QString &publishError) {
                     return publishStagedFileSet(
                         { StagedFilePublication(stage, target) },
                         publishError);
                 },
                 [&]() {
                     cacheUpdated = QFile::exists(target)
                         && !QFile::exists(stage)
                         && readAll(target) == contents;
                 },
                 error),
             qPrintable(error));

    QVERIFY(error.isEmpty());
    QVERIFY(cacheUpdated);
}

void TestAtomicActivitySave::stagedFileSetRollsBackOnMiddleFailure()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QList<StagedFilePublication> files;
    for (int index = 0; index < 3; ++index) {
        const QString stage =
            dir.filePath(QStringLiteral(".stage-%1").arg(index));
        const QString target =
            dir.filePath(QStringLiteral("target-%1.json").arg(index));
        writeFixture(stage, QByteArray("activity-")
                                + QByteArray::number(index));
        files.append(StagedFilePublication(stage, target));
    }

    int publishCalls = 0;
    const AtomicPublishFunction publish =
        [&](const QString &stage, const QString &target,
            bool &temporaryMoved, QString &publishError) {
            ++publishCalls;
            if (publishCalls == 2) {
                temporaryMoved = false;
                publishError =
                    QStringLiteral("injected middle publication failure");
                return false;
            }
            return publishAtomicNew(
                stage, target, temporaryMoved, publishError);
        };

    QString error;
    QVERIFY(!publishStagedFileSet(files, error, publish));
    QCOMPARE(publishCalls, 2);
    QVERIFY(error.contains(QStringLiteral("injected middle")));
    for (const StagedFilePublication &file : files) {
        QVERIFY(!QFile::exists(file.first));
        QVERIFY(!QFile::exists(file.second));
    }
}

void TestAtomicActivitySave::
stagedFileSetRollsBackPartiallyPublishedFailure()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QList<StagedFilePublication> files;
    for (int index = 0; index < 3; ++index) {
        const QString stage =
            dir.filePath(QStringLiteral(".stage-%1").arg(index));
        const QString target =
            dir.filePath(QStringLiteral("target-%1.json").arg(index));
        writeFixture(stage, QByteArray("activity-")
                                + QByteArray::number(index));
        files.append(StagedFilePublication(stage, target));
    }

    int publishCalls = 0;
    const AtomicPublishFunction publish =
        [&](const QString &stage, const QString &target,
            bool &temporaryMoved, QString &publishError) {
            ++publishCalls;
            if (publishCalls == 2) {
                if (!QFile::copy(stage, target)) {
                    publishError =
                        QStringLiteral("cannot inject partial publication");
                    return false;
                }
                temporaryMoved = true;
                publishError =
                    QStringLiteral("injected post-publication failure");
                return false;
            }
            return publishAtomicNew(
                stage, target, temporaryMoved, publishError);
        };

    QString error;
    QVERIFY(!publishStagedFileSet(files, error, publish));
    QCOMPARE(publishCalls, 2);
    QVERIFY(error.contains(QStringLiteral("post-publication")));
    for (const StagedFilePublication &file : files) {
        QVERIFY(!QFile::exists(file.first));
        QVERIFY(!QFile::exists(file.second));
    }
}

void TestAtomicActivitySave::stagedFileSetCleansStagingOnCollision()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString stage = dir.filePath(QStringLiteral(".activity.stage"));
    const QString target = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray existing("existing activity");
    writeFixture(stage, QByteArray("replacement"));
    writeFixture(target, existing);

    int publishCalls = 0;
    const AtomicPublishFunction publish =
        [&](const QString &source, const QString &destination,
            bool &temporaryMoved, QString &publishError) {
            ++publishCalls;
            return publishAtomicNew(
                source, destination, temporaryMoved, publishError);
        };

    QString error;
    QVERIFY(!publishStagedFileSet(
        { StagedFilePublication(stage, target) }, error, publish));
    QCOMPARE(publishCalls, 0);
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFile::exists(stage));
    QCOMPARE(readAll(target), existing);
}


void TestAtomicActivitySave::stagedFileSetRejectsUnsafePathGraphs()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString samePath =
        dir.filePath(QStringLiteral("same.json"));
    const QByteArray sameContents("must not be deleted");
    writeFixture(samePath, sameContents);

    QString error;
    QVERIFY(!publishStagedFileSet(
        { StagedFilePublication(samePath, samePath) }, error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readAll(samePath), sameContents);

    const QString firstStage =
        dir.filePath(QStringLiteral("first.stage"));
    const QString secondStage =
        dir.filePath(QStringLiteral("second.stage"));
    const QString finalTarget =
        dir.filePath(QStringLiteral("final.json"));
    const QByteArray firstContents("first staging data");
    const QByteArray secondContents("second staging data");
    writeFixture(firstStage, firstContents);
    writeFixture(secondStage, secondContents);

    error.clear();
    QVERIFY(!publishStagedFileSet(
        {
            StagedFilePublication(firstStage, secondStage),
            StagedFilePublication(secondStage, finalTarget)
        },
        error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readAll(firstStage), firstContents);
    QCOMPARE(readAll(secondStage), secondContents);
    QVERIFY(!QFile::exists(finalTarget));
}

void TestAtomicActivitySave::
stagedFileSetRejectsInvalidAndDuplicateStages()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString validStage =
        dir.filePath(QStringLiteral("valid.stage"));
    const QString missingStage =
        dir.filePath(QStringLiteral("missing.stage"));
    const QString firstTarget =
        dir.filePath(QStringLiteral("first.json"));
    const QString secondTarget =
        dir.filePath(QStringLiteral("second.json"));
    writeFixture(validStage, QByteArray("valid staging data"));

    QString error;
    QVERIFY(!publishStagedFileSet(
        {
            StagedFilePublication(validStage, firstTarget),
            StagedFilePublication(missingStage, secondTarget)
        },
        error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFile::exists(validStage));
    QVERIFY(!QFile::exists(firstTarget));
    QVERIFY(!QFile::exists(secondTarget));

    const QString duplicateStage =
        dir.filePath(QStringLiteral("duplicate.stage"));
    const QByteArray duplicateContents("duplicate staging data");
    writeFixture(duplicateStage, duplicateContents);
    error.clear();
    QVERIFY(!publishStagedFileSet(
        {
            StagedFilePublication(duplicateStage, firstTarget),
            StagedFilePublication(duplicateStage, secondTarget)
        },
        error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readAll(duplicateStage), duplicateContents);
    QVERIFY(!QFile::exists(firstTarget));
    QVERIFY(!QFile::exists(secondTarget));

    const QString firstDuplicateTargetStage =
        dir.filePath(QStringLiteral("first-duplicate-target.stage"));
    const QString secondDuplicateTargetStage =
        dir.filePath(QStringLiteral("second-duplicate-target.stage"));
    writeFixture(firstDuplicateTargetStage, QByteArray("first"));
    writeFixture(secondDuplicateTargetStage, QByteArray("second"));
    error.clear();
    QVERIFY(!publishStagedFileSet(
        {
            StagedFilePublication(firstDuplicateTargetStage, firstTarget),
            StagedFilePublication(secondDuplicateTargetStage, firstTarget)
        },
        error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFile::exists(firstDuplicateTargetStage));
    QVERIFY(!QFile::exists(secondDuplicateTargetStage));
    QVERIFY(!QFile::exists(firstTarget));

    const QString backing =
        dir.filePath(QStringLiteral("backing.stage"));
    const QString linkedStage =
        dir.filePath(QStringLiteral("linked.stage"));
    const QByteArray backingContents("backing data");
    writeFixture(backing, backingContents);
    if (!QFile::link(backing, linkedStage)
        || !QFileInfo(linkedStage).isSymLink()) {
        QSKIP("Symbolic links are unavailable on this platform");
    }
    error.clear();
    QVERIFY(!publishStagedFileSet(
        { StagedFilePublication(linkedStage, firstTarget) }, error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFile::exists(linkedStage));
    QCOMPARE(readAll(backing), backingContents);
    QVERIFY(!QFile::exists(firstTarget));
}

void TestAtomicActivitySave::
stagedFileSetReleasesLocksAfterLockFailure()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString firstStage =
        dir.filePath(QStringLiteral("first.stage"));
    const QString secondStage =
        dir.filePath(QStringLiteral("second.stage"));
    const QString firstTarget =
        dir.filePath(QStringLiteral("a.json"));
    const QString secondTarget =
        dir.filePath(QStringLiteral("z.json"));
    writeFixture(firstStage, QByteArray("first"));
    writeFixture(secondStage, QByteArray("second"));

    QLockFile heldLock(atomicFileLockPath(secondTarget));
    heldLock.setStaleLockTime(0);
    QVERIFY(heldLock.tryLock(0));

    QString error;
    QVERIFY(!publishStagedFileSet(
        {
            StagedFilePublication(firstStage, firstTarget),
            StagedFilePublication(secondStage, secondTarget)
        },
        error));
    QVERIFY(error.contains(QStringLiteral("being saved"),
                           Qt::CaseInsensitive));
    QVERIFY(!QFile::exists(firstStage));
    QVERIFY(!QFile::exists(secondStage));

    QLockFile firstLock(atomicFileLockPath(firstTarget));
    firstLock.setStaleLockTime(0);
    QVERIFY(firstLock.tryLock(0));
}

void TestAtomicActivitySave::stagedFileSetFinalizesWhileTargetsAreLocked()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString stage = dir.filePath(QStringLiteral("activity.stage"));
    const QString target = dir.filePath(QStringLiteral("activity.json"));
    writeFixture(stage, QByteArray("activity"));

    bool sawPublishedTarget = false;
    bool sawHeldTargetLock = false;
    int finalizeCalls = 0;
    const std::function<bool(QString &)> finalize =
        [&](QString &finalizeError) {
            ++finalizeCalls;
            sawPublishedTarget =
                readAll(target) == QByteArray("activity");

            QLockFile competingLock(atomicFileLockPath(target));
            competingLock.setStaleLockTime(0);
            sawHeldTargetLock = !competingLock.tryLock(0);
            if (!sawPublishedTarget || !sawHeldTargetLock) {
                finalizeError =
                    QStringLiteral("target was not durably locked");
                return false;
            }
            return true;
        };

    QString error;
    QVERIFY2(publishStagedFileSet(
                 { StagedFilePublication(stage, target) }, error,
                 publishAtomicNew, finalize),
             qPrintable(error));
    QCOMPARE(finalizeCalls, 1);
    QVERIFY(sawPublishedTarget);
    QVERIFY(sawHeldTargetLock);
    QVERIFY(!QFile::exists(stage));
    QCOMPARE(readAll(target), QByteArray("activity"));
}

void TestAtomicActivitySave::stagedFileSetReturnsSuccessfulFinalizerWarning()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString stage = dir.filePath(QStringLiteral("activity.stage"));
    const QString target = dir.filePath(QStringLiteral("activity.json"));
    writeFixture(stage, QByteArray("activity"));

    const std::function<bool(QString &)> finalize =
        [](QString &finalizeError) {
            finalizeError = QStringLiteral("recovery warning");
            return true;
        };

    QString error;
    QVERIFY(publishStagedFileSet(
        { StagedFilePublication(stage, target) }, error,
        publishAtomicNew, finalize));
    QCOMPARE(error, QStringLiteral("recovery warning"));
    QVERIFY(!QFile::exists(stage));
    QCOMPARE(readAll(target), QByteArray("activity"));
}

void TestAtomicActivitySave::stagedFileSetRollsBackWhenFinalizerFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QList<StagedFilePublication> files;
    for (int index = 0; index < 2; ++index) {
        const QString stage =
            dir.filePath(QStringLiteral("activity-%1.stage").arg(index));
        const QString target =
            dir.filePath(QStringLiteral("activity-%1.json").arg(index));
        writeFixture(stage, QByteArray("activity-")
                                + QByteArray::number(index));
        files.append(StagedFilePublication(stage, target));
    }

    int finalizeCalls = 0;
    bool sawAllTargets = true;
    const std::function<bool(QString &)> finalize =
        [&](QString &finalizeError) {
            ++finalizeCalls;
            for (const StagedFilePublication &file : std::as_const(files)) {
                sawAllTargets = sawAllTargets
                    && QFile::exists(file.second);
            }
            finalizeError = QStringLiteral("injected finalization failure");
            return false;
        };

    QString error;
    QVERIFY(!publishStagedFileSet(
        files, error, publishAtomicNew, finalize));
    QCOMPARE(finalizeCalls, 1);
    QVERIFY(sawAllTargets);
    QVERIFY(error.contains(QStringLiteral("finalization failure")));
    for (const StagedFilePublication &file : std::as_const(files)) {
        QVERIFY(!QFile::exists(file.first));
        QVERIFY(!QFile::exists(file.second));
    }
}

void TestAtomicActivitySave::lockSetKeepsCaseDistinctPaths()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString upperPath =
        dir.filePath(QStringLiteral("Activity.json"));
    const QString lowerPath =
        dir.filePath(QStringLiteral("activity.json"));
    writeFixture(upperPath, QByteArray("upper"));
    if (QFileInfo(lowerPath).exists()) {
        QSKIP("The test filesystem is case-insensitive");
    }

    AtomicFileLockSet locks;
    QString error;
    QVERIFY2(locks.lock({ upperPath, lowerPath }, error),
             qPrintable(error));

    QLockFile upperLock(atomicFileLockPath(upperPath));
    upperLock.setStaleLockTime(0);
    QLockFile lowerLock(atomicFileLockPath(lowerPath));
    lowerLock.setStaleLockTime(0);
    QVERIFY(!upperLock.tryLock(0));
    QVERIFY(!lowerLock.tryLock(0));
}

void TestAtomicActivitySave::atomicWriterPreservesConcurrentNewTarget()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray concurrent("concurrent activity");
    bool sawCreateNew = false;
    const AtomicFileWriterFactory factory =
        [&](const QString &target, AtomicFileMode mode) {
            sawCreateNew = mode == AtomicFileMode::CreateNew;
            writeFixture(target, concurrent);
            return std::unique_ptr<AtomicFileWriter>(
                new NewAtomicFileWriter(target));
        };

    QString error;
    QVERIFY(!writeFileAtomically(
        path, QByteArray("replacement"), factory, error, false));
    QVERIFY(sawCreateNew);
    QVERIFY(!error.isEmpty());
    QCOMPARE(readAll(path), concurrent);
}

void TestAtomicActivitySave::jsonWriterFailurePreservesOriginal_data()
{
    failurePreservesOriginalAndDirty_data();
}

void TestAtomicActivitySave::jsonWriterFailurePreservesOriginal()
{
    QFETCH(int, failurePoint);
    QFETCH(QString, expectedError);
    const FailurePoint failure = static_cast<FailurePoint>(failurePoint);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("original activity bytes");
    writeFixture(path, original);

    RideFile ride(QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    ride.setTag(QStringLiteral("Notes"), QStringLiteral("atomic test"));
    const AtomicFileWriterFactory factory = [failure](const QString &target, AtomicFileMode) {
        return std::unique_ptr<AtomicFileWriter>(
            new FaultInjectingWriter(target, failure));
    };
    JsonFileReader reader(factory);
    QFile target(path);
    QString error;

    QVERIFY(!reader.writeRideFile(nullptr, &ride, target, error));
    QVERIFY(!error.isEmpty());
    QVERIFY2(error.contains(expectedError, Qt::CaseInsensitive),
             qPrintable(error));
    QCOMPARE(readAll(path), original);
}

void TestAtomicActivitySave::jsonWriterKeepsUtf8BomAndRoundTrips()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));

    RideFile ride(QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    const QString notes = QString::fromUtf8("Hyv\xC3\xA4 harjoitus");
    ride.setTag(QStringLiteral("Notes"), notes);

    JsonFileReader reader;
    QFile target(path);
    QString error;
    QVERIFY2(reader.writeRideFile(nullptr, &ride, target, error),
             qPrintable(error));

    const QByteArray bytes = readAll(path);
    QVERIFY(bytes.startsWith(QByteArray::fromHex("efbbbf")));

    QStringList parseErrors;
    QFile input(path);
    std::unique_ptr<RideFile> parsed(
        reader.openRideFile(input, parseErrors));
    QVERIFY2(parsed != nullptr,
             qPrintable(parseErrors.join(QStringLiteral("; "))));
    QCOMPARE(parsed->getTag(QStringLiteral("Notes"), QString()), notes);
}

void TestAtomicActivitySave::jsonReaderRejectsMalformedPayload()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("malformed.json"));
    writeFixture(path, QByteArray::fromHex("efbbbf")
                           + QByteArray("{\"RIDE\":{\"STARTTIME\":"));

    JsonFileReader reader;
    QStringList errors;
    QFile input(path);
    std::unique_ptr<RideFile> parsed(reader.openRideFile(input, errors));

    QVERIFY(parsed == nullptr);
    QVERIFY(!errors.isEmpty());
}

void TestAtomicActivitySave::jsonWriterRejectsOpenTarget()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("original activity bytes");
    writeFixture(path, original);

    RideFile ride(QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    JsonFileReader reader;
    QFile target(path);
    QVERIFY(target.open(QIODevice::ReadWrite));
    QString error;

    QVERIFY(!reader.writeRideFile(nullptr, &ride, target, error));
    QVERIFY(error.contains(QStringLiteral("open"), Qt::CaseInsensitive));
    QVERIFY(target.seek(0));
    QCOMPARE(target.readAll(), original);
    target.close();
}

void TestAtomicActivitySave::
jsonWriterRejectsExistingTargetWhenReplacementDisabled()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("existing activity");
    writeFixture(path, original);

    int factoryCalls = 0;
    const AtomicFileWriterFactory factory =
        [&](const QString &target, AtomicFileMode mode) {
            ++factoryCalls;
            return qSaveFileWriterFactory()(target, mode);
        };
    JsonFileReader reader(factory);
    RideFile ride(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    QFile target(path);
    QString error;

    QVERIFY(!reader.writeRideFile(
        nullptr, &ride, target, error, false));
    QVERIFY(error.contains(QStringLiteral("exists"),
                           Qt::CaseInsensitive));
    QCOMPARE(factoryCalls, 0);
    QCOMPARE(readAll(path), original);
}

void TestAtomicActivitySave::jsonReaderKeepsLegacyLatin1Compatibility()
{
    RideFile ride(QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    ride.setTag(QStringLiteral("Notes"), QStringLiteral("placeholder"));

    JsonFileReader reader;
    QByteArray bytes =
        reader.toByteArray(nullptr, &ride, true, true, true, true);
    QByteArray latin1Notes("Hyv");
    latin1Notes.append(char(0xe4));
    QVERIFY(bytes.contains("placeholder"));
    bytes.replace("placeholder", latin1Notes);

    QStringList errors;
    std::unique_ptr<RideFile> parsed(reader.fromByteArray(bytes, errors));
    QVERIFY2(parsed != nullptr, qPrintable(errors.join(QStringLiteral("; "))));
    QCOMPARE(parsed->getTag(QStringLiteral("Notes"), QString()),
             QString::fromLatin1(latin1Notes));
}

void TestAtomicActivitySave::
jsonReaderPreservesUtf8ReplacementCharacter()
{
    RideFile ride(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    const QString notes =
        QStringLiteral("before ")
        + QString(QChar::ReplacementCharacter)
        + QStringLiteral(" after");
    ride.setTag(QStringLiteral("Notes"), notes);

    JsonFileReader reader;
    const QByteArray bytes =
        reader.toByteArray(nullptr, &ride, true, true, true, true);
    QStringList errors;
    std::unique_ptr<RideFile> parsed(
        reader.fromByteArray(bytes, errors));

    QVERIFY2(parsed != nullptr,
             qPrintable(errors.join(QStringLiteral("; "))));
    QCOMPARE(parsed->getTag(QStringLiteral("Notes"), QString()),
             notes);
}

void TestAtomicActivitySave::jsonReaderPreservesLargeInteger()
{
    RideFile ride(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    RideFilePoint sample;
    sample.secs = 1.0;
    ride.appendPoint(sample);

    JsonFileReader reader;
    QByteArray bytes =
        reader.toByteArray(nullptr, &ride, true, true, true, true);
    const QByteArray original("\"SECS\":1");
    const QByteArray large("\"SECS\":3000000000");
    QVERIFY(bytes.contains(original));
    bytes.replace(original, large);

    QStringList errors;
    std::unique_ptr<RideFile> parsed(
        reader.fromByteArray(bytes, errors));
    QVERIFY2(parsed != nullptr,
             qPrintable(errors.join(QStringLiteral("; "))));
    QCOMPARE(parsed->dataPoints().size(), 1);
    QCOMPARE(parsed->dataPoints().constFirst()->secs,
             3000000000.0);
}

void TestAtomicActivitySave::jsonReaderPreservesPositiveExponent()
{
    RideFile ride(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    RideFilePoint sample;
    sample.secs = 1.0;
    ride.appendPoint(sample);

    JsonFileReader reader;
    QByteArray bytes =
        reader.toByteArray(nullptr, &ride, true, true, true, true);
    const QByteArray original("\"SECS\":1");
    const QByteArray exponent("\"SECS\":1e+10");
    QVERIFY(bytes.contains(original));
    bytes.replace(original, exponent);

    QStringList errors;
    std::unique_ptr<RideFile> parsed(
        reader.fromByteArray(bytes, errors));
    QVERIFY2(parsed != nullptr,
             qPrintable(errors.join(QStringLiteral("; "))));
    QCOMPARE(parsed->dataPoints().size(), 1);
    QCOMPARE(parsed->dataPoints().constFirst()->secs, 1e10);
}

void TestAtomicActivitySave::jsonRoundTripsReferenceAndSampleState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));

    RideFile ride(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    RideFilePoint sample;
    sample.secs = 12.5;
    sample.tcore = 38.125;
    sample.interval = 7;
    ride.appendPoint(sample);
    ride.setDataPresent(RideFile::tcore, true);
    ride.setDataPresent(RideFile::interval, true);

    RideFilePoint reference;
    reference.secs = 42.0;
    reference.watts = 321.0;
    reference.cad = 91.0;
    reference.hr = 155.0;
    ride.appendReference(reference);

    JsonFileReader reader;
    QFile target(path);
    QString error;
    QVERIFY2(reader.writeRideFile(nullptr, &ride, target, error),
             qPrintable(error));

    QStringList parseErrors;
    QFile input(path);
    std::unique_ptr<RideFile> parsed(
        reader.openRideFile(input, parseErrors));
    QVERIFY2(parsed != nullptr,
             qPrintable(parseErrors.join(QStringLiteral("; "))));
    QCOMPARE(parsed->referencePoints().size(), 1);
    const RideFilePoint *parsedReference =
        parsed->referencePoints().constFirst();
    QCOMPARE(parsedReference->secs, reference.secs);
    QCOMPARE(parsedReference->watts, reference.watts);
    QCOMPARE(parsedReference->cad, reference.cad);
    QCOMPARE(parsedReference->hr, reference.hr);
    QCOMPARE(parsed->dataPoints().size(), 1);
    const RideFilePoint *parsedSample =
        parsed->dataPoints().constFirst();
    QCOMPARE(parsedSample->tcore, sample.tcore);
    QCOMPARE(parsedSample->interval, sample.interval);
    QVERIFY(parsed->areDataPresent()->tcore);
    QVERIFY(parsed->areDataPresent()->interval);
}

void TestAtomicActivitySave::saveHelpersRejectInvalidOperations()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    RideFile ride(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    ActivitySaveOperations operations;
    QString error;

    QVERIFY(!saveActivityTransaction(
        nullptr, nullptr, path, operations, error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!saveActivityTransaction(
        nullptr, &ride, path, operations, error));
    QVERIFY(error.contains(QStringLiteral("writer"),
                           Qt::CaseInsensitive));

    operations.writerFactory = qSaveFileWriterFactory();
    QVERIFY(!saveActivityTransaction(
        nullptr, &ride, path, operations, error));
    QVERIFY(error.contains(QStringLiteral("complete"),
                           Qt::CaseInsensitive));

    operations.finalize = [](QString &) { return true; };
    operations.markClean = []() {};
    operations.stage = [](RideFile *, QString &) { return false; };
    QVERIFY(!saveActivityTransaction(
        nullptr, &ride, path, operations, error));
    QVERIFY(error.contains(QStringLiteral("processor"),
                           Qt::CaseInsensitive));
    QVERIFY(!QFile::exists(path));

    RideFile original(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    RideFile replacement(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    RideItem current(&original, nullptr);
    RideItem candidate(&replacement, nullptr);
    int saveCalls = 0;
    const ActivityCandidateSave save =
        [&](RideItem *, QString &) {
            ++saveCalls;
            return false;
        };

    QVERIFY(!saveActivityCandidate(
        nullptr, &candidate, &replacement, save, error));
    QVERIFY(!saveActivityCandidate(
        &current, &current, &replacement, save, error));
    QVERIFY(!saveActivityCandidate(
        &current, &candidate, &original, save, error));
    QCOMPARE(saveCalls, 0);

    error.clear();
    QVERIFY(!saveActivityCandidate(
        &current, &candidate, &replacement, save, error));
    QCOMPARE(saveCalls, 1);
    QVERIFY(!error.isEmpty());
    QCOMPARE(current.ride(false), &original);
    QCOMPARE(candidate.ride(false), &replacement);
}

void TestAtomicActivitySave::successfulTransactionPreservesLiveObjectState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));

    ObservableRideFile ride(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    RideFilePoint sample;
    sample.secs = 1.1234567890123;
    sample.watts = 287.987654321;
    ride.appendPoint(sample);
    RideFilePoint *const liveSample = ride.dataPoints().constFirst();

    RideFilePoint reference;
    reference.secs = 15.25;
    reference.watts = 350.5;
    ride.appendReference(reference);
    RideFilePoint *const liveReference =
        ride.referencePoints().constFirst();

    CIQinfo info(QStringLiteral("test-application"), 4, 2);
    info.fields.append(CIQfield(
        QStringLiteral("record"), QStringLiteral("test-field"),
        7, 8, QStringLiteral("float32"), QStringLiteral("unit"),
        10, 3));
    ride.addCIQ(info);

    bool markedClean = false;
    ActivitySaveOperations operations;
    operations.writerFactory = qSaveFileWriterFactory();
    operations.finalize = [](QString &) { return true; };
    operations.markClean = [&]() {
        markedClean = true;
        ride.markSaved();
    };
    operations.timestamp =
        QDateTime(QDate(2026, 7, 6), QTime(9, 0));

    QString error;
    QVERIFY2(saveActivityTransaction(
                 nullptr, &ride, path, operations, error),
             qPrintable(error));
    QVERIFY(markedClean);
    QCOMPARE(ride.dataPoints().constFirst(), liveSample);
    QCOMPARE(liveSample->secs, sample.secs);
    QCOMPARE(liveSample->watts, sample.watts);
    QCOMPARE(ride.referencePoints().constFirst(), liveReference);
    QCOMPARE(liveReference->secs, reference.secs);
    QCOMPARE(liveReference->watts, reference.watts);
    QCOMPARE(ride.ciqinfo().size(), 1);
    QCOMPARE(ride.ciqinfo().constFirst().appid, info.appid);
    QCOMPARE(ride.ciqinfo().constFirst().fields.size(), 1);
}

void TestAtomicActivitySave::transactionPreservesZeroValuedSeriesPresence()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("activity.json"));

    ObservableRideFile ride(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    RideFilePoint point;
    point.secs = 1.0;
    ride.appendPoint(point);
    ride.setDataPresent(RideFile::hr, true);

    bool markedClean = false;
    ActivitySaveOperations operations;
    operations.writerFactory = qSaveFileWriterFactory();
    operations.finalize = [](QString &) { return true; };
    operations.markClean = [&]() {
        markedClean = true;
        ride.markSaved();
    };
    operations.timestamp =
        QDateTime(QDate(2026, 7, 6), QTime(9, 0));

    QString error;
    QVERIFY2(saveActivityTransaction(
                 nullptr, &ride, path, operations, error),
             qPrintable(error));
    QVERIFY(markedClean);
    QVERIFY(readAll(path).contains(QByteArrayLiteral("\"HR\":0")));
}

void TestAtomicActivitySave::processorFailureSkipsWriteAndKeepsDirty()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString targetPath = dir.filePath(QStringLiteral("activity.json"));

    ObservableRideFile ride(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    ride.setTag(QStringLiteral("State"), QStringLiteral("original"));

    int writerCreations = 0;
    int stageRuns = 0;
    int finalizeRuns = 0;
    int markedClean = 0;
    bool stageSawLiveRide = false;

    ActivitySaveOperations operations;
    operations.writerFactory =
        [&](const QString &target, AtomicFileMode) {
            ++writerCreations;
            return std::unique_ptr<AtomicFileWriter>(
                new FaultInjectingWriter(target, FailurePoint::None));
        };
    operations.stage = [&](RideFile *activity, QString &stageError) {
        ++stageRuns;
        stageSawLiveRide = activity == &ride;
        activity->setTag(QStringLiteral("State"), QStringLiteral("staged"));
        stageError = QStringLiteral("injected processor failure");
        return false;
    };
    operations.finalize = [&](QString &) {
        ++finalizeRuns;
        return true;
    };
    operations.markClean = [&]() { ++markedClean; };

    QString error;
    QVERIFY(!saveActivityTransaction(
        nullptr, &ride, targetPath, operations, error));
    QCOMPARE(error, QStringLiteral("injected processor failure"));
    QCOMPARE(stageRuns, 1);
    QCOMPARE(writerCreations, 0);
    QCOMPARE(finalizeRuns, 0);
    QCOMPARE(markedClean, 0);
    QVERIFY(stageSawLiveRide);
    QVERIFY(!QFile::exists(targetPath));
    QCOMPARE(ride.getTag(QStringLiteral("State"), QString()),
             QStringLiteral("staged"));
}

void TestAtomicActivitySave::failedSaveRestoresHistoryUntilCommit()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sourcePath = dir.filePath(QStringLiteral("activity.fit"));
    const QString targetPath = dir.filePath(QStringLiteral("activity.json"));
    const QByteArray original("original activity bytes");
    writeFixture(sourcePath, original);

    ObservableRideFile ride(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    ride.setTag(QStringLiteral("Processor Runs"), QStringLiteral("0"));
    const QString initialHistory = QStringLiteral("before\n");
    ride.setTag(QStringLiteral("Change History"), initialHistory);

    bool dirty = true;
    int savedSignals = 0;
    QObject::connect(&ride, &RideFile::saved, &ride, [&]() {
        dirty = false;
        ++savedSignals;
    });

    int stageRuns = 0;
    int finalizeRuns = 0;
    bool stageSawOnlyLiveRide = true;
    bool injectFinalizeFailure = true;
    ActivitySaveOperations operations;
    operations.stage = [&](RideFile *activity, QString &) {
        stageSawOnlyLiveRide =
            stageSawOnlyLiveRide && activity == &ride;
        ++stageRuns;
        const int runs =
            activity->getTag(QStringLiteral("Processor Runs"),
                             QStringLiteral("0")).toInt() + 1;
        activity->setTag(QStringLiteral("Processor Runs"),
                         QString::number(runs));
        return true;
    };
    operations.finalize = [&](QString &stepError) {
        ++finalizeRuns;
        if (injectFinalizeFailure) {
            stepError = QStringLiteral("injected finalize failure");
            return false;
        }
        return finalizeActivityFileReplacement(
            sourcePath, targetPath, true, stepError);
    };
    operations.markClean = [&]() { ride.markSaved(); };
    operations.timestamp =
        QDateTime(QDate(2026, 7, 6), QTime(9, 0));

    QString error;
    const QList<FailurePoint> writerFailures = {
        FailurePoint::Open, FailurePoint::ShortWrite,
        FailurePoint::Flush, FailurePoint::Commit,
        FailurePoint::CorruptCommit, FailurePoint::MissingCommit
    };
    for (const FailurePoint failure : writerFailures) {
        operations.writerFactory =
            [failure](const QString &target, AtomicFileMode) {
                return std::unique_ptr<AtomicFileWriter>(
                    new FaultInjectingWriter(target, failure));
            };

        QVERIFY(!saveActivityTransaction(
            nullptr, &ride, targetPath, operations, error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(readAll(sourcePath), original);
        QVERIFY(!QFile::exists(targetPath));
        QCOMPARE(ride.getTag(QStringLiteral("Change History"), QString()),
                 initialHistory);
    }
    QCOMPARE(ride.getTag(QStringLiteral("Processor Runs"), QString()).toInt(),
             writerFailures.size());
    QVERIFY(dirty);
    QVERIFY(stageSawOnlyLiveRide);
    QCOMPARE(stageRuns, writerFailures.size());
    QCOMPARE(finalizeRuns, 0);
    QCOMPARE(savedSignals, 0);

    operations.writerFactory =
        [](const QString &target, AtomicFileMode) {
            return std::unique_ptr<AtomicFileWriter>(
                new FaultInjectingWriter(target, FailurePoint::None));
        };

    QVERIFY(!saveActivityTransaction(
        nullptr, &ride, targetPath, operations, error));
    QCOMPARE(error, QStringLiteral("injected finalize failure"));
    QCOMPARE(readAll(sourcePath), original);
    QVERIFY(QFile::exists(targetPath));
    QCOMPARE(ride.getTag(QStringLiteral("Change History"), QString()),
             initialHistory);
    QCOMPARE(ride.getTag(QStringLiteral("Processor Runs"), QString()).toInt(),
             writerFailures.size() + 1);
    QVERIFY(dirty);
    QCOMPARE(stageRuns, writerFailures.size() + 1);
    QCOMPARE(finalizeRuns, 1);
    QCOMPARE(savedSignals, 0);

    injectFinalizeFailure = false;
    QVERIFY2(saveActivityTransaction(
                 nullptr, &ride, targetPath, operations, error),
             qPrintable(error));
    QVERIFY(error.isEmpty());
    QVERIFY(!dirty);
    QCOMPARE(stageRuns, writerFailures.size() + 2);
    QCOMPARE(finalizeRuns, 2);
    QCOMPARE(savedSignals, 1);
    QCOMPARE(ride.getTag(QStringLiteral("Processor Runs"), QString()).toInt(),
             writerFailures.size() + 2);
    QCOMPARE(ride.getTag(QStringLiteral("Change History"), QString())
                 .count(QStringLiteral("Changes on ")),
             1);
    QVERIFY(!QFile::exists(sourcePath));
    QCOMPARE(readAll(sourcePath + QStringLiteral(".bak")), original);

    JsonFileReader reader;
    QStringList parseErrors;
    QFile input(targetPath);
    std::unique_ptr<RideFile> parsed(reader.openRideFile(input, parseErrors));
    QVERIFY2(parsed != nullptr,
             qPrintable(parseErrors.join(QStringLiteral("; "))));
    QCOMPARE(
        parsed->getTag(QStringLiteral("Processor Runs"), QString()).toInt(),
        writerFailures.size() + 2);
    QCOMPARE(parsed->getTag(QStringLiteral("Change History"), QString()),
             ride.getTag(QStringLiteral("Change History"), QString()));
}

void TestAtomicActivitySave::
candidateSaveTransfersOnlyAfterSuccess()
{
    RideFile original(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    RideFile replacement(
        QDateTime(QDate(2026, 7, 6), QTime(8, 30)), 1.0);
    RideItem current(&original, nullptr);
    current.path = QStringLiteral("/source");
    current.fileName = QStringLiteral("original.json");
    current.setDirty(false);
    RideItem candidate(&replacement, nullptr);

    bool failureSawIsolatedCandidate = false;
    QString error;
    QVERIFY(!saveActivityCandidate(
        &current, &candidate, &replacement,
        [&](RideItem *saveItem, QString &saveError) {
            failureSawIsolatedCandidate =
                saveItem == &candidate
                && saveItem->ride(false) == &replacement
                && current.ride(false) == &original;
            saveError = QStringLiteral("injected candidate save failure");
            return false;
        },
        error));
    QVERIFY(failureSawIsolatedCandidate);
    QCOMPARE(error, QStringLiteral("injected candidate save failure"));
    QCOMPARE(current.ride(false), &original);
    QCOMPARE(current.path, QStringLiteral("/source"));
    QCOMPARE(current.fileName, QStringLiteral("original.json"));
    QVERIFY(!current.isDirty());
    QCOMPARE(candidate.ride(false), &replacement);

    bool successSawIsolatedCandidate = false;
    QVERIFY(saveActivityCandidate(
        &current, &candidate, &replacement,
        [&](RideItem *saveItem, QString &) {
            successSawIsolatedCandidate =
                saveItem == &candidate
                && current.ride(false) == &original;
            saveItem->path = QStringLiteral("/committed");
            saveItem->fileName = QStringLiteral("replacement.json");
            return true;
        },
        error));
    QVERIFY(successSawIsolatedCandidate);
    QVERIFY(error.isEmpty());
    QCOMPARE(current.ride(false), &replacement);
    QCOMPARE(current.path, QStringLiteral("/committed"));
    QCOMPARE(current.fileName, QStringLiteral("replacement.json"));
    QVERIFY(!current.isDirty());
    QVERIFY(candidate.ride(false) == nullptr);
}

void TestAtomicActivitySave::
defaultProcessorFailurePreservesFileAndDirtyState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime startTime(QDate(2026, 7, 6), QTime(8, 30));
    const QString fileName =
        activityFileName(startTime, QStringLiteral("json"));
    const QString path = dir.filePath(fileName);
    const QByteArray original("original activity bytes");
    writeFixture(path, original);

    RideFile ride(startTime, 1.0);
    RideItem item(&ride, nullptr);
    item.path = dir.path();
    item.fileName = fileName;
    item.setDirty(true);

    resetAtomicActivitySaveProcessorStub();
    setAtomicActivitySaveProcessorFailure(true);

    QString error;
    QVERIFY(!MainWindow::saveSilent(nullptr, &item, &error));
    QCOMPARE(atomicActivitySaveProcessorCalls(), 1);
    QVERIFY(error.contains(QStringLiteral("processor"),
                           Qt::CaseInsensitive));
    QCOMPARE(readAll(path), original);
    QVERIFY(item.isDirty());
    QCOMPARE(item.fileName, fileName);
    QVERIFY(!ride.tags().contains(QStringLiteral("Change History")));
}

void TestAtomicActivitySave::mainWindowConvertsSourceAfterCommit()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime startTime(QDate(2026, 7, 6), QTime(8, 30));
    const QString sourceName = QStringLiteral("import.fit");
    const QString targetName =
        activityFileName(startTime, QStringLiteral("json"));
    const QString sourcePath = dir.filePath(sourceName);
    const QString targetPath = dir.filePath(targetName);
    const QString notesPath = dir.filePath(QStringLiteral("import.notes"));
    const QByteArray original("original imported activity");
    writeFixture(sourcePath, original);
    writeFixture(notesPath, QByteArray("notes"));

    RideFile ride(startTime, 1.0);
    ride.setTag(QStringLiteral("State"), QStringLiteral("converted"));
    RideItem item(&ride, nullptr);
    item.path = dir.path();
    item.fileName = sourceName;
    item.setDirty(true);

    bool writerCreated = false;
    AtomicFileMode writerMode = AtomicFileMode::ReplaceExisting;
    const AtomicFileWriterFactory realWriter = qSaveFileWriterFactory();
    ActivitySaveOperations operations;
    operations.writerFactory =
        [&](const QString &path, AtomicFileMode mode) {
            writerCreated = true;
            writerMode = mode;
            return realWriter(path, mode);
        };

    QString error;
    QVERIFY2(MainWindow::saveSilent(
                 nullptr, &item, &error, &operations),
             qPrintable(error));
    QVERIFY(error.isEmpty());
    QVERIFY(writerCreated);
    QCOMPARE(writerMode, AtomicFileMode::CreateNew);
    QVERIFY(!QFile::exists(sourcePath));
    QCOMPARE(readAll(sourcePath + QStringLiteral(".bak")), original);
    QVERIFY(QFile::exists(targetPath));
    QVERIFY(!QFile::exists(notesPath));
    QCOMPARE(item.fileName, targetName);
    QVERIFY(!item.isDirty());

    JsonFileReader reader;
    QStringList parseErrors;
    QFile input(targetPath);
    std::unique_ptr<RideFile> parsed(
        reader.openRideFile(input, parseErrors));
    QVERIFY2(parsed != nullptr,
             qPrintable(parseErrors.join(QStringLiteral("; "))));
    QCOMPARE(parsed->getTag(QStringLiteral("State"), QString()),
             QStringLiteral("converted"));
}

void TestAtomicActivitySave::mainWindowRenamesJsonAfterCommit()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime startTime(QDate(2026, 7, 6), QTime(8, 30));
    const QString sourceName = QStringLiteral("old-name.json");
    const QString targetName =
        activityFileName(startTime, QStringLiteral("json"));
    const QString sourcePath = dir.filePath(sourceName);
    const QString targetPath = dir.filePath(targetName);
    const QByteArray original("old json activity");
    writeFixture(sourcePath, original);

    RideFile ride(startTime, 1.0);
    RideItem item(&ride, nullptr);
    item.path = dir.path();
    item.fileName = sourceName;
    item.setDirty(true);

    AtomicFileMode writerMode = AtomicFileMode::ReplaceExisting;
    const AtomicFileWriterFactory realWriter = qSaveFileWriterFactory();
    ActivitySaveOperations operations;
    operations.writerFactory =
        [&](const QString &path, AtomicFileMode mode) {
            writerMode = mode;
            return realWriter(path, mode);
        };

    QString error;
    QVERIFY2(MainWindow::saveSilent(
                 nullptr, &item, &error, &operations),
             qPrintable(error));
    QCOMPARE(writerMode, AtomicFileMode::CreateNew);
    QVERIFY(!QFile::exists(sourcePath));
    QVERIFY(!QFile::exists(sourcePath + QStringLiteral(".bak")));
    QVERIFY(QFile::exists(targetPath));
    QCOMPARE(item.fileName, targetName);
    QVERIFY(!item.isDirty());
}

void TestAtomicActivitySave::mainWindowRejectsTargetCollision()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime startTime(QDate(2026, 7, 6), QTime(8, 30));
    const QString sourceName = QStringLiteral("import.fit");
    const QString targetName =
        activityFileName(startTime, QStringLiteral("json"));
    const QString sourcePath = dir.filePath(sourceName);
    const QString targetPath = dir.filePath(targetName);
    const QByteArray sourceBytes("source activity");
    const QByteArray targetBytes("unrelated existing activity");
    writeFixture(sourcePath, sourceBytes);
    writeFixture(targetPath, targetBytes);

    RideFile ride(startTime, 1.0);
    RideItem item(&ride, nullptr);
    item.path = dir.path();
    item.fileName = sourceName;
    item.setDirty(true);

    int writerCreations = 0;
    int stageRuns = 0;
    ActivitySaveOperations operations;
    operations.writerFactory =
        [&](const QString &path, AtomicFileMode mode) {
            ++writerCreations;
            return qSaveFileWriterFactory()(path, mode);
        };
    operations.stage = [&](RideFile *, QString &) {
        ++stageRuns;
        return true;
    };

    QString error;
    QVERIFY(!MainWindow::saveSilent(
        nullptr, &item, &error, &operations));
    QVERIFY(error.contains(QStringLiteral("already exists"),
                           Qt::CaseInsensitive));
    QCOMPARE(writerCreations, 0);
    QCOMPARE(stageRuns, 0);
    QCOMPARE(readAll(sourcePath), sourceBytes);
    QCOMPARE(readAll(targetPath), targetBytes);
    QVERIFY(!QFile::exists(sourcePath + QStringLiteral(".bak")));
    QCOMPARE(item.fileName, sourceName);
    QVERIFY(item.isDirty());
}

void TestAtomicActivitySave::mainWindowHoldsSourceAndTargetLocks()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime startTime(QDate(2026, 7, 6), QTime(8, 30));
    const QString sourceName = QStringLiteral("locked.fit");
    const QString targetName =
        activityFileName(startTime, QStringLiteral("json"));
    const QString sourcePath = dir.filePath(sourceName);
    const QString targetPath = dir.filePath(targetName);
    writeFixture(sourcePath, QByteArray("source activity"));

    RideFile ride(startTime, 1.0);
    RideItem item(&ride, nullptr);
    item.path = dir.path();
    item.fileName = sourceName;
    item.setDirty(true);

    bool sourceWasLocked = false;
    bool targetWasLocked = false;
    ActivitySaveOperations operations;
    operations.writerFactory = qSaveFileWriterFactory();
    operations.stage = [&](RideFile *, QString &) {
        QLockFile sourceLock(atomicFileLockPath(sourcePath));
        sourceLock.setStaleLockTime(0);
        sourceWasLocked = !sourceLock.tryLock(0);
        QLockFile targetLock(atomicFileLockPath(targetPath));
        targetLock.setStaleLockTime(0);
        targetWasLocked = !targetLock.tryLock(0);
        return true;
    };

    QString error;
    QVERIFY2(MainWindow::saveSilent(
                 nullptr, &item, &error, &operations),
             qPrintable(error));
    QVERIFY(sourceWasLocked);
    QVERIFY(targetWasLocked);
}

void TestAtomicActivitySave::
mainWindowFinalizeFailureRemainsRetryable()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime startTime(QDate(2026, 7, 6), QTime(8, 30));
    const QString sourceName = QStringLiteral("retry.fit");
    const QString targetName =
        activityFileName(startTime, QStringLiteral("json"));
    const QString sourcePath = dir.filePath(sourceName);
    const QString targetPath = dir.filePath(targetName);
    const QByteArray original("source activity");
    writeFixture(sourcePath, original);

    RideFile ride(startTime, 1.0);
    RideItem item(&ride, nullptr);
    item.path = dir.path();
    item.fileName = sourceName;
    item.setDirty(true);

    bool removeSourceAfterCommit = true;
    bool sourceWasRemoved = false;
    ActivitySaveOperations operations;
    operations.writerFactory =
        [&](const QString &target, AtomicFileMode) {
            return std::unique_ptr<AtomicFileWriter>(
                new FaultInjectingWriter(
                    target, FailurePoint::None, [&]() {
                        if (removeSourceAfterCommit) {
                            sourceWasRemoved = QFile::remove(sourcePath);
                        }
                    }));
        };

    QString error;
    QVERIFY(!MainWindow::saveSilent(
        nullptr, &item, &error, &operations));
    QVERIFY(sourceWasRemoved);
    QVERIFY(!error.isEmpty());
    QVERIFY(item.isDirty());
    QCOMPARE(item.fileName, sourceName);
    QVERIFY(!QFile::exists(targetPath));

    writeFixture(sourcePath, original);
    removeSourceAfterCommit = false;
    QVERIFY2(MainWindow::saveSilent(
                 nullptr, &item, &error, &operations),
             qPrintable(error));
    QVERIFY(error.isEmpty());
    QCOMPARE(item.fileName, targetName);
    QVERIFY(!item.isDirty());
    QVERIFY(QFile::exists(targetPath));
    QCOMPARE(readAll(sourcePath + QStringLiteral(".bak")), original);
}

void TestAtomicActivitySave::mainWindowRejectsSourceChangedDuringSave()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime startTime(QDate(2026, 7, 6), QTime(8, 30));
    const QString sourceName = QStringLiteral("changed.fit");
    const QString targetName =
        activityFileName(startTime, QStringLiteral("json"));
    const QString sourcePath = dir.filePath(sourceName);
    const QString targetPath = dir.filePath(targetName);
    const QByteArray original("original source activity");
    const QByteArray concurrent("concurrently changed source");
    writeFixture(sourcePath, original);

    RideFile ride(startTime, 1.0);
    RideItem item(&ride, nullptr);
    item.path = dir.path();
    item.fileName = sourceName;
    item.setDirty(true);

    bool sourceWasChanged = false;
    ActivitySaveOperations operations;
    operations.writerFactory =
        [&](const QString &target, AtomicFileMode) {
            return std::unique_ptr<AtomicFileWriter>(
                new FaultInjectingWriter(
                    target, FailurePoint::None, [&]() {
                        QFile source(sourcePath);
                        sourceWasChanged =
                            source.open(QIODevice::WriteOnly
                                        | QIODevice::Truncate)
                            && source.write(concurrent)
                                == concurrent.size()
                            && source.flush();
                    }));
        };

    QString error;
    QVERIFY(!MainWindow::saveSilent(
        nullptr, &item, &error, &operations));
    QVERIFY(sourceWasChanged);
    QVERIFY(!error.isEmpty());
    QCOMPARE(readAll(sourcePath), concurrent);
    QVERIFY(!QFile::exists(sourcePath + QStringLiteral(".bak")));
    QVERIFY(!QFile::exists(targetPath));
    QCOMPARE(item.fileName, sourceName);
    QVERIFY(item.isDirty());
}

void TestAtomicActivitySave::mainWindowSaveSilentPropagatesFailure()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime startTime(QDate(2026, 7, 6), QTime(8, 30));
    const QString fileName =
        activityFileName(startTime, QStringLiteral("json"));
    const QString path = dir.filePath(fileName);
    const QString notesPath =
        dir.filePath(QFileInfo(fileName).baseName()
                     + QStringLiteral(".notes"));
    const QByteArray original("original activity bytes");
    writeFixture(path, original);
    writeFixture(notesPath, QByteArray("notes"));

    RideFile ride(startTime, 1.0);
    ride.setTag(QStringLiteral("State"), QStringLiteral("original"));
    RideItem item(&ride, nullptr);
    item.path = dir.path();
    item.fileName = fileName;
    item.setDirty(true);

    int stageRuns = 0;
    ActivitySaveOperations operations;
    operations.writerFactory =
        [](const QString &target, AtomicFileMode) {
            return std::unique_ptr<AtomicFileWriter>(
                new FaultInjectingWriter(target, FailurePoint::Commit));
        };
    operations.stage = [&](RideFile *activity, QString &) {
        ++stageRuns;
        activity->setTag(QStringLiteral("State"), QStringLiteral("processed"));
        return true;
    };

    QString error;
    QVERIFY(!MainWindow::saveSilent(
        nullptr, &item, &error, &operations));
    QVERIFY(error.contains(QStringLiteral("commit"), Qt::CaseInsensitive));
    QCOMPARE(readAll(path), original);
    QVERIFY(QFile::exists(notesPath));
    QVERIFY(item.isDirty());
    QCOMPARE(item.fileName, fileName);
    QCOMPARE(ride.getTag(QStringLiteral("State"), QString()),
             QStringLiteral("processed"));
    QCOMPARE(stageRuns, 1);
}

void TestAtomicActivitySave::
mainWindowSaveSilentPreservesUppercaseJsonPath()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDateTime startTime(QDate(2026, 7, 6), QTime(8, 30));
    const QString fileName =
        activityFileName(startTime, QStringLiteral("JSON"));
    const QString sourcePath = dir.filePath(fileName);
    writeFixture(sourcePath, QByteArray("old activity"));

    RideFile ride(startTime, 1.0);
    ride.setTag(QStringLiteral("State"), QStringLiteral("original"));
    RideItem item(&ride, nullptr);
    item.path = dir.path();
    item.fileName = fileName;
    item.setDirty(true);

    int stageRuns = 0;
    bool stageSawLiveRide = false;
    QString writerPath;
    AtomicFileMode writerMode = AtomicFileMode::CreateNew;
    const AtomicFileWriterFactory realWriter = qSaveFileWriterFactory();
    ActivitySaveOperations operations;
    operations.writerFactory =
        [&](const QString &path, AtomicFileMode mode) {
            writerPath = path;
            writerMode = mode;
            return realWriter(path, mode);
        };
    operations.stage = [&](RideFile *activity, QString &) {
        ++stageRuns;
        stageSawLiveRide = activity == &ride;
        activity->setTag(QStringLiteral("State"), QStringLiteral("processed"));
        return true;
    };

    QString error;
    QVERIFY2(MainWindow::saveSilent(
                 nullptr, &item, &error, &operations),
             qPrintable(error));
    QVERIFY(error.isEmpty());
    QCOMPARE(item.fileName, fileName);
    QVERIFY(QFile::exists(sourcePath));
    QCOMPARE(writerPath, sourcePath);
    QCOMPARE(writerMode, AtomicFileMode::ReplaceExisting);
    QVERIFY(!item.isDirty());
    QCOMPARE(stageRuns, 1);
    QVERIFY(stageSawLiveRide);
    QCOMPARE(ride.getTag(QStringLiteral("State"), QString()),
             QStringLiteral("processed"));

    JsonFileReader reader;
    QStringList parseErrors;
    QFile input(sourcePath);
    std::unique_ptr<RideFile> parsed(
        reader.openRideFile(input, parseErrors));
    QVERIFY2(parsed != nullptr,
             qPrintable(parseErrors.join(QStringLiteral("; "))));
    QCOMPARE(parsed->getTag(QStringLiteral("State"), QString()),
             QStringLiteral("processed"));
}

void TestAtomicActivitySave::
mainWindowSaveRideSingleDialogPropagatesResult()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    RideItem item(nullptr, nullptr);
    item.path = dir.path();
    item.fileName = QStringLiteral("activity.gc");

    int saveCalls = 0;
    bool receivedExpectedActivity = false;
    QString reportedError;
    MainWindow::SaveRideDialogOperations operations;
    operations.saveActivities =
        [&](const QList<RideItem *> &items, QString &) {
            ++saveCalls;
            receivedExpectedActivity =
                items.size() == 1 && items.first() == &item;
            return true;
        };
    operations.reportError =
        [&](const QString &error) { reportedError = error; };

    QVERIFY(MainWindow::saveRideSingleDialog(
        nullptr, &item, &operations));
    QCOMPARE(saveCalls, 1);
    QVERIFY(receivedExpectedActivity);
    QVERIFY(reportedError.isEmpty());

    operations.saveActivities =
        [&](const QList<RideItem *> &, QString &error) {
            ++saveCalls;
            error = QStringLiteral("injected collection save failure");
            return false;
        };
    QVERIFY(!MainWindow::saveRideSingleDialog(
        nullptr, &item, &operations));
    QCOMPARE(saveCalls, 2);
    QCOMPARE(reportedError,
             QStringLiteral("injected collection save failure"));
}

void TestAtomicActivitySave::saveSingleDialogSaveAndAbandon()
{
    RideItem item(nullptr, nullptr);
    item.fileName = QStringLiteral("activity.fit");
    item.setDirty(true);

    TestSaveSingleDialog saveDialog(&item);
    saveDialog.setResult(42);
    saveDialog.saveResult = false;
    saveDialog.saveClicked();
    QCOMPARE(saveDialog.result(), 42);
    QVERIFY(!saveDialog.mayProceed());
    QCOMPARE(saveDialog.saveCalls, 1);
    QCOMPARE(saveDialog.errorReports, 1);
    QVERIFY(item.isDirty());

    saveDialog.saveResult = true;
    saveDialog.saveClicked();
    QCOMPARE(saveDialog.result(), int(QDialog::Accepted));
    QVERIFY(saveDialog.mayProceed());
    QCOMPARE(saveDialog.saveCalls, 2);

    item.setDirty(true);
    TestSaveSingleDialog abandonDialog(&item);
    abandonDialog.setResult(42);
    abandonDialog.abandonClicked();
    QCOMPARE(abandonDialog.result(), int(QDialog::Rejected));
    QVERIFY(abandonDialog.mayProceed());
    QVERIFY(!item.isDirty());
}

void TestAtomicActivitySave::
saveOnExitDialogStopsUntilAllSelectedSave()
{
    RideItem first(nullptr, nullptr);
    first.fileName = QStringLiteral("first.json");
    first.setDirty(true);
    RideItem second(nullptr, nullptr);
    second.fileName = QStringLiteral("second.json");
    second.setDirty(true);

    TestSaveOnExitDialog dialog({&first, &second});
    dialog.results.insert(&first, true);
    dialog.results.insert(&second, false);
    dialog.setResult(42);

    dialog.saveClicked();
    QCOMPARE(dialog.result(), 42);
    QCOMPARE(dialog.calls, QList<RideItem *>({&first, &second}));
    QVERIFY(!first.isDirty());
    QVERIFY(second.isDirty());

    dialog.calls.clear();
    dialog.results.insert(&second, true);
    dialog.saveClicked();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.calls, QList<RideItem *>({&second}));
    QVERIFY(!second.isDirty());
}

void TestAtomicActivitySave::
saveOnExitDialogDefersSkippedStateUntilSuccess()
{
    RideItem skipped(nullptr, nullptr);
    skipped.fileName = QStringLiteral("skipped.json");
    skipped.setDirty(true);
    RideItem failing(nullptr, nullptr);
    failing.fileName = QStringLiteral("failing.json");
    failing.setDirty(true);

    TestSaveOnExitDialog dialog({&skipped, &failing});
    QTableWidget *const table = dialog.findChild<QTableWidget *>();
    QVERIFY(table != nullptr);
    QCheckBox *const skippedCheckBox =
        qobject_cast<QCheckBox *>(table->cellWidget(0, 0));
    QVERIFY(skippedCheckBox != nullptr);
    skippedCheckBox->setChecked(false);
    dialog.results.insert(&failing, false);
    dialog.setResult(42);

    dialog.saveClicked();
    QCOMPARE(dialog.result(), 42);
    QCOMPARE(dialog.calls, QList<RideItem *>({&failing}));
    QVERIFY(!skipped.skipsave);
    QVERIFY(skipped.isDirty());
    QVERIFY(failing.isDirty());

    dialog.calls.clear();
    dialog.results.insert(&failing, true);
    dialog.saveClicked();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.calls, QList<RideItem *>({&failing}));
    QVERIFY(skipped.skipsave);
    QVERIFY(skipped.isDirty());
    QVERIFY(!failing.isDirty());
}

void TestAtomicActivitySave::rideCacheSaveActivityPropagatesFailure()
{
    RideItem item(nullptr, nullptr);
    item.fileName = QStringLiteral("activity.json");
    item.setDirty(true);

    int saveCalls = 0;
    int notifications = 0;
    RideCache::SaveActivityFunction save =
        [&](Context *, RideItem *, QString *error) {
            ++saveCalls;
            *error = QStringLiteral("injected cache save failure");
            return false;
        };
    const RideCache::ActivitySavedFunction notify =
        [&](RideItem *savedItem) {
            QCOMPARE(savedItem, &item);
            ++notifications;
        };

    QString error;
    QVERIFY(!RideCache::saveActivity(
        nullptr, &item, error, save, notify));
    QCOMPARE(error, QStringLiteral("injected cache save failure"));
    QCOMPARE(saveCalls, 1);
    QCOMPARE(notifications, 0);
    QVERIFY(item.isDirty());

    save = [&](Context *, RideItem *savedItem, QString *) {
        ++saveCalls;
        savedItem->setDirty(false);
        return true;
    };
    QVERIFY(RideCache::saveActivity(
        nullptr, &item, error, save, notify));
    QVERIFY(error.isEmpty());
    QCOMPARE(saveCalls, 2);
    QCOMPARE(notifications, 1);
    QVERIFY(!item.isDirty());

    QVERIFY(RideCache::saveActivity(
        nullptr, &item, error, save, notify));
    QCOMPARE(saveCalls, 2);
    QCOMPARE(notifications, 1);

    QVERIFY(!RideCache::saveActivity(
        nullptr, nullptr, error, save, notify));
    QVERIFY(!error.isEmpty());
}

void TestAtomicActivitySave::rideCacheSaveActivitiesAggregatesFailures()
{
    RideItem first(nullptr, nullptr);
    first.fileName = QStringLiteral("first.json");
    first.setDirty(true);
    RideItem second(nullptr, nullptr);
    second.fileName = QStringLiteral("second.json");
    second.setDirty(true);
    RideItem third(nullptr, nullptr);
    third.fileName = QStringLiteral("third.json");
    third.setDirty(true);
    RideItem clean(nullptr, nullptr);
    clean.fileName = QStringLiteral("clean.json");
    clean.setDirty(false);

    bool failBatch = true;
    int saveCalls = 0;
    QList<RideItem *> notifications;
    const RideCache::SaveActivityFunction save =
        [&](Context *, RideItem *item, QString *error) {
            ++saveCalls;
            if (failBatch && (item == &first || item == &third)) {
                *error = item == &first
                    ? QStringLiteral("injected first failure")
                    : QStringLiteral("injected third failure");
                return false;
            }
            item->setDirty(false);
            return true;
        };
    const RideCache::ActivitySavedFunction notify =
        [&](RideItem *item) { notifications.append(item); };

    QString error = QStringLiteral("stale error");
    QVERIFY(!RideCache::saveActivities(
        nullptr, {&first, &second, &third, &clean}, error, save, notify));
    QCOMPARE(saveCalls, 3);
    QCOMPARE(notifications, QList<RideItem *>({&second}));
    QVERIFY(error.contains(first.fileName));
    QVERIFY(error.contains(third.fileName));
    QVERIFY(error.contains(QStringLiteral("injected first failure")));
    QVERIFY(error.contains(QStringLiteral("injected third failure")));
    QVERIFY(first.isDirty());
    QVERIFY(!second.isDirty());
    QVERIFY(third.isDirty());

    failBatch = false;
    QVERIFY(RideCache::saveActivities(
        nullptr, {&first, &second, &third, &clean}, error, save, notify));
    QVERIFY(error.isEmpty());
    QCOMPARE(saveCalls, 5);
    QCOMPARE(notifications,
             QList<RideItem *>({&second, &first, &third}));
    QVERIFY(!first.isDirty());
    QVERIFY(!third.isDirty());

    QVERIFY(!RideCache::saveActivities(
        nullptr, {nullptr}, error, save, notify));
    QVERIFY(error.contains(QStringLiteral("unknown activity")));
    QCOMPARE(saveCalls, 5);
}

QTEST_MAIN(TestAtomicActivitySave)

#include "testAtomicActivitySave.moc"
