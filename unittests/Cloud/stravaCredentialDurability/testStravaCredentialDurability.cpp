#include "Cloud/StravaCredentialDurability.h"
#include "Cloud/StravaSettingsCommit.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace {

std::mutex &transitionMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::function<void(const QString &)> &transitionAction()
{
    static std::function<void(const QString &)> action;
    return action;
}

void setTransitionAction(std::function<void(const QString &)> action)
{
    const std::lock_guard<std::mutex> lock(transitionMutex());
    transitionAction() = std::move(action);
}

struct TransitionActionReset
{
    ~TransitionActionReset()
    {
        setTransitionAction({});
    }
};

} // namespace

void stravaCredentialDurabilityTransitionReached(const char *transition)
{
    std::function<void(const QString &)> action;
    {
        const std::lock_guard<std::mutex> lock(transitionMutex());
        action = transitionAction();
    }
    if (action) action(QString::fromLatin1(transition));
}

namespace {

using namespace StravaCredentialDurability;
using StravaTokenPublication::PublicationMode;
using StravaTokenPublication::PublicationResult;
using StravaTokenPublication::PublicationStatus;
using StravaTokenPublication::TokenPair;

struct FakeSecureStore
{
    TokenPair active;
    QString timestamp;
    QString authorizationState = QStringLiteral("active");
    bool remoteGrantUncertain = false;
    QString pendingTransaction;
    int failAccessWrites = 0;
    int failTimestampWrites = 0;
    int failCleanupWrites = 0;
    int refreshCalls = 0;
    int cleanupCalls = 0;
    int revisionTouches = 0;
    bool currentReadable = true;
    bool pendingTransactionReadable = true;
    std::function<void()> refreshWriteHook;
    std::function<void()> stateWriteHook;
    std::function<void()> metadataRefreshHook;
    std::function<void()> currentReadHook;
    QString authorizationRevision = QStringLiteral("revision-1");

    TokenPair activeValue() const
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return active;
    }

    QString timestampValue() const
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return timestamp;
    }

    QString authorizationStateValue() const
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return authorizationState;
    }

    bool remoteGrantUncertainValue() const
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return remoteGrantUncertain;
    }

    QString pendingTransactionValue() const
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return pendingTransaction;
    }

    void setStateWriteHook(std::function<void()> hook)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        stateWriteHook = std::move(hook);
    }

    StorageCallbacks callbacks()
    {
        StorageCallbacks result = {
            [this] {
                TokenPairReadResult result;
                std::function<void()> hook;
                {
                    const std::lock_guard<std::mutex> lock(mutex);
                    result = {currentReadable, active};
                    hook = currentReadHook;
                }
                if (hook) hook();
                return result;
            },
            [this] { return timestampValue(); },
            [this] { return authorizationStateValue(); },
            [this] { return remoteGrantUncertainValue(); },
            [this] { return pendingTransactionValue(); },
            [this](const QString &value) {
                const std::lock_guard<std::mutex> lock(mutex);
                pendingTransaction = value;
                return true;
            },
            [this] {
                const std::lock_guard<std::mutex> lock(mutex);
                pendingTransaction.clear();
                return true;
            },
            [this](const QString &value) {
                std::function<void()> hook;
                {
                    const std::lock_guard<std::mutex> lock(mutex);
                    hook = refreshWriteHook;
                }
                if (hook) hook();
                const std::lock_guard<std::mutex> lock(mutex);
                active.refreshToken = value;
                return true;
            },
            [this](const QString &value) {
                const std::lock_guard<std::mutex> lock(mutex);
                if (failAccessWrites > 0) {
                    --failAccessWrites;
                    return false;
                }
                active.accessToken = value;
                return true;
            },
            [this](const QString &value) {
                const std::lock_guard<std::mutex> lock(mutex);
                if (failTimestampWrites > 0) {
                    --failTimestampWrites;
                    return false;
                }
                timestamp = value;
                return true;
            },
            [this](const QString &value) {
                std::function<void()> hook;
                {
                    const std::lock_guard<std::mutex> lock(mutex);
                    hook = stateWriteHook;
                }
                if (hook) hook();
                const std::lock_guard<std::mutex> lock(mutex);
                authorizationState = value;
                return true;
            },
            [this](bool value) {
                const std::lock_guard<std::mutex> lock(mutex);
                remoteGrantUncertain = value;
                return true;
            },
            [] { return true; },
            [this] {
                std::function<void()> hook;
                {
                    const std::lock_guard<std::mutex> lock(mutex);
                    ++refreshCalls;
                    hook = metadataRefreshHook;
                }
                if (hook) hook();
                return true;
            },
            [this] {
                const std::lock_guard<std::mutex> lock(mutex);
                ++cleanupCalls;
                if (failCleanupWrites > 0) {
                    --failCleanupWrites;
                    return false;
                }
                authorizationState = QStringLiteral("revoked");
                remoteGrantUncertain = false;
                return true;
            },
            [] { return true; },
            [] { return QString(); },
            {}
        };
        result.touchAuthorizationRevision = [this] {
            const std::lock_guard<std::mutex> lock(mutex);
            ++revisionTouches;
            return true;
        };
        result.readPendingTransactionForRecovery = [this] {
            const std::lock_guard<std::mutex> lock(mutex);
            return PendingTransactionReadResult{
                pendingTransactionReadable,
                pendingTransaction
            };
        };
        result.readAuthorizationRevision = [this] {
            const std::lock_guard<std::mutex> lock(mutex);
            return authorizationRevision;
        };
        return result;
    }

    mutable std::mutex mutex;
};

QString createTransactionParent(QTemporaryDir &temporary)
{
    const QString path = temporary.filePath(QStringLiteral("config"));
    if (!QDir().mkpath(path)) return {};
    return QFileInfo(path).canonicalFilePath();
}

Publication replacementPublication(const QString &label)
{
    Publication publication;
    publication.expectedRefreshToken =
        QStringLiteral("refresh-old");
    publication.replacement = {
        QStringLiteral("access-") + label,
        QStringLiteral("refresh-") + label
    };
    publication.refreshedAt =
        QStringLiteral("2026-08-05T12:00:00.000+03:00");
    publication.mode = PublicationMode::CompareAndSwap;
    publication.activatesAuthorization = true;
    publication.clearsRemoteGrantUncertainty = true;
    return publication;
}

class SharedDurableStore
{
public:
    explicit SharedDurableStore(QString path)
        : path_(std::move(path))
    {
    }

    QString read(const QString &key) const
    {
        QSettings settings(path_, QSettings::IniFormat);
        return settings.value(key).toString();
    }

    bool write(const QString &key, const QVariant &value) const
    {
        QSettings settings(path_, QSettings::IniFormat);
        if (value.isValid()) settings.setValue(key, value);
        else settings.remove(key);
        settings.sync();
        return settings.status() == QSettings::NoError;
    }

    StorageCallbacks callbacks() const
    {
        const SharedDurableStore copy = *this;
        StorageCallbacks result = {
            [copy] {
                return TokenPairReadResult{
                    true,
                    TokenPair{
                        copy.read(QStringLiteral("access")),
                        copy.read(QStringLiteral("refresh"))
                    }
                };
            },
            [copy] { return copy.read(QStringLiteral("timestamp")); },
            [copy] {
                return copy.read(QStringLiteral("authorization_state"));
            },
            [copy] {
                return copy.read(QStringLiteral("uncertain"))
                    == QStringLiteral("true");
            },
            [copy] { return copy.read(QStringLiteral("pending")); },
            [copy](const QString &value) {
                return copy.write(QStringLiteral("pending"), value);
            },
            [copy] {
                return copy.write(
                    QStringLiteral("pending"), QVariant());
            },
            [copy](const QString &value) {
                return copy.write(QStringLiteral("refresh"), value);
            },
            [copy](const QString &value) {
                return copy.write(QStringLiteral("access"), value);
            },
            [copy](const QString &value) {
                return copy.write(QStringLiteral("timestamp"), value);
            },
            [copy](const QString &value) {
                return copy.write(
                    QStringLiteral("authorization_state"), value);
            },
            [copy](bool value) {
                return copy.write(
                    QStringLiteral("uncertain"),
                    value ? QStringLiteral("true")
                          : QStringLiteral("false"));
            },
            [] { return true; },
            [] { return true; },
            [copy] {
                return copy.write(
                    QStringLiteral("authorization_state"),
                    QStringLiteral("revoked"))
                    && copy.write(
                        QStringLiteral("uncertain"),
                        QStringLiteral("false"));
            },
            [] { return true; },
            [] { return QString(); },
            {}
        };
        result.readPendingTransactionForRecovery = [copy] {
            return PendingTransactionReadResult{
                true,
                copy.read(QStringLiteral("pending"))
            };
        };
        return result;
    }

private:
    QString path_;
};

bool writeChildMarker(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(contents) == contents.size()
        && file.flush();
}

int runGrantChild(const QStringList &arguments)
{
    if (arguments.size() != 9) return 64;
    const QString transactionParent = arguments.at(2);
    const QString accountKey = arguments.at(3);
    const SharedDurableStore vault(arguments.at(4));
    const SharedDurableStore provider(arguments.at(5));
    const QString operation = arguments.at(6);
    const int holdMs = arguments.at(7).toInt();
    const QString resultPath = arguments.at(8);
    const MutationKind kind = operation == QStringLiteral("remove")
        ? MutationKind::Revocation
        : operation == QStringLiteral("oauth")
            ? MutationKind::Authorization
            : MutationKind::Refresh;

    StorageCallbacks vaultCallbacks = vault.callbacks();
    if (operation == QStringLiteral("crash-refresh")) {
        const auto durableWrite = vaultCallbacks.writeRefreshToken;
        vaultCallbacks.writeRefreshToken =
            [durableWrite](const QString &value) {
                if (!durableWrite(value)) return false;
                std::_Exit(86);
            };
    }
    Coordinator coordinator(
        accountKey, transactionParent, std::move(vaultCallbacks));
    QString error;
    std::shared_ptr<Mutation> mutation = coordinator.begin(
        kind, 5000, error);
    if (!mutation) return 66;
    if (!mutation->markPendingState(error)) return 67;

    StoredState observed;
    if (!mutation->readStoredState(observed, error)) return 68;
    if (!writeChildMarker(resultPath + QStringLiteral(".acquired"),
                          QByteArrayLiteral("acquired\n"))) return 69;

    QThread::msleep(static_cast<unsigned long>(holdMs));
    if (!mutation->markCommitUnknown(error)) return 70;

    bool succeeded = false;
    if (operation == QStringLiteral("refresh")
        || operation == QStringLiteral("crash-refresh")) {
        if (provider.read(QStringLiteral("refresh"))
            != observed.credentials.refreshToken) return 71;
        if (!provider.write(
                QStringLiteral("refresh"),
                QStringLiteral("refresh-new"))) return 72;
        Publication publication =
            replacementPublication(QStringLiteral("new"));
        succeeded = mutation->publish(publication).isSuccess();
    } else if (operation == QStringLiteral("remove")) {
        if (provider.read(QStringLiteral("refresh"))
            != observed.credentials.refreshToken) return 73;
        if (!provider.write(QStringLiteral("refresh"), QVariant()))
            return 74;
        const StorageCallbacks storage = vault.callbacks();
        succeeded = storage.writeRefreshToken(QString())
            && storage.writeAccessToken(QString())
            && storage.writeTimestamp(QString())
            && storage.writeAuthorizationState(
                QStringLiteral("revoked"))
            && storage.writeRemoteGrantUncertain(false)
            && storage.sync()
            && mutation->finishCommit(error);
    } else if (operation == QStringLiteral("oauth")) {
        if (!provider.write(
                QStringLiteral("refresh"),
                QStringLiteral("refresh-oauth"))) return 75;
        Publication publication;
        publication.replacement = {
            QStringLiteral("access-oauth"),
            QStringLiteral("refresh-oauth")
        };
        publication.refreshedAt = QStringLiteral("oauth-timestamp");
        publication.mode = PublicationMode::Authoritative;
        publication.activatesAuthorization = true;
        publication.clearsRemoteGrantUncertainty = true;
        succeeded = mutation->publish(publication).isSuccess();
    } else {
        return 76;
    }
    if (!succeeded) return 77;

    const QByteArray observedToken =
        observed.credentials.refreshToken.isEmpty()
            ? QByteArrayLiteral("-")
            : observed.credentials.refreshToken.toUtf8();
    const QByteArray result = QByteArray::number(mutation->generation())
        + ' ' + observedToken + '\n';
    return writeChildMarker(resultPath, result) ? 0 : 78;
}

} // namespace

class TestStravaCredentialDurability : public QObject
{
    Q_OBJECT

private slots:
    void partialPublicationRecoversAfterRestart_data();
    void partialPublicationRecoversAfterRestart();
    void processDeathDuringPublicationRecoversDurably();
    void recoveryDistinguishesUnstartedAndUnknownRemoteMutation();
    void unknownRevocationRecoveryPreservesVaultGrantAndAllowsCleanup();
    void revocationConflictRecoveryPreservesNewerGrantUntilExplicitCleanup();
    void publicationConflictRecoveryRetiresFenceForExplicitRetry();
    void legacyRemotePendingJournalMigratesFailClosed();
    void independentProcessesSerializeAndFenceGenerations();
    void guiOriginatedCredentialWorkRunsOffApplicationThread();
    void startedCredentialCommitReturnsTrackedPendingWithinDeadline();
    void latePendingStateWriteCannotCrossMutationGeneration();
    void leaseRefreshesMetadataBeforeCapturingRollbackState();
    void revocationCleanupRemainsRecoverableUntilDurable();
    void unstartedRemoteCommitRestoresPreviousAuthorization();
    void authorizationTransitionsPublishRevisionEvidence();
    void transientTokenReadDoesNotRetirePublicationJournal();
    void transientPendingPackageReadDoesNotRetireJournal();
    void transientRevocationReadDoesNotRetireJournal();
    void mutationReadFailsClosedWhenCredentialsAreUnavailable();
    void localRevocationRecoveryResumesBeforeFirstDelete();
    void confirmedRemoteRevocationRecoveryResumesBeforeFirstDelete();
    void coherentReadWaitsForAccountMutation();
    void coherentReadRejectsRevisionChange();
    void journalWriteDoesNotFollowReplacedNamespacePath();
    void accountLockDoesNotFollowReplacedNamespacePath();
};

void TestStravaCredentialDurability::
revocationConflictRecoveryPreservesNewerGrantUntilExplicitCleanup()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent =
        createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());

    FakeSecureStore store;
    store.active = {
        QStringLiteral("access-old"),
        QStringLiteral("refresh-old")
    };
    QString error;
    {
        Coordinator coordinator(
            QStringLiteral("athlete"),
            transactionParent,
            store.callbacks());
        const std::shared_ptr<Mutation> removal = coordinator.begin(
            MutationKind::Revocation, 1000, error);
        QVERIFY2(removal, qPrintable(error));
        QVERIFY(removal->markPendingState(error));
        QVERIFY(removal->markCommitUnknown(error));
        QVERIFY(removal->markLocalCommitStarted(
            QStringLiteral("refresh-old"),
            PublicationMode::CompareAndSwap,
            error));
        QVERIFY(store.pendingTransactionValue().contains(
            QStringLiteral("refresh-old")));
        QFile journal(QDir(transactionParent).filePath(
            QStringLiteral(
                ".strava-credential-durability/state.json")));
        QVERIFY(journal.open(QIODevice::ReadOnly));
        const QByteArray journalContents = journal.readAll();
        QVERIFY(!journalContents.contains("access-old"));
        QVERIFY(!journalContents.contains("refresh-old"));
    }

    store.active = {
        QStringLiteral("access-newer"),
        QStringLiteral("refresh-newer")
    };
    Coordinator restarted(
        QStringLiteral("athlete"),
        transactionParent,
        store.callbacks());
    const RecoveryResult conflicted = restarted.recover(1000);
    QCOMPARE(conflicted.status, RecoveryStatus::Recovered);
    QCOMPARE(
        store.activeValue().accessToken,
        QStringLiteral("access-newer"));
    QCOMPARE(
        store.activeValue().refreshToken,
        QStringLiteral("refresh-newer"));
    QCOMPARE(
        store.authorizationStateValue(),
        QStringLiteral("revocation_pending"));
    QVERIFY(store.remoteGrantUncertainValue());
    QVERIFY(store.pendingTransactionValue().isEmpty());

    {
        const std::shared_ptr<Mutation> cleanup = restarted.begin(
            MutationKind::Revocation, 1000, error);
        QVERIFY2(cleanup, qPrintable(error));
        QVERIFY(cleanup->markPendingState(error));
        QVERIFY(cleanup->markLocalCommitStarted(
            QString(),
            PublicationMode::Authoritative,
            error));
    }
    QCOMPARE(
        restarted.recover(1000).status,
        RecoveryStatus::Recovered);
    QVERIFY(store.activeValue().accessToken.isEmpty());
    QVERIFY(store.activeValue().refreshToken.isEmpty());
    QCOMPARE(
        store.authorizationStateValue(),
        QStringLiteral("revoked"));
}

void TestStravaCredentialDurability::
publicationConflictRecoveryRetiresFenceForExplicitRetry()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent =
        createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());

    FakeSecureStore store;
    store.active = {
        QStringLiteral("access-old"),
        QStringLiteral("refresh-old")
    };
    QString error;
    {
        Coordinator coordinator(
            QStringLiteral("athlete"),
            transactionParent,
            store.callbacks());
        const std::shared_ptr<Mutation> refresh = coordinator.begin(
            MutationKind::Refresh, 1000, error);
        QVERIFY2(refresh, qPrintable(error));
        QVERIFY(refresh->markPendingState(error));
        QVERIFY(refresh->armPublication(
            replacementPublication(QStringLiteral("replacement")),
            error));
        store.active = {
            QStringLiteral("access-newer"),
            QStringLiteral("refresh-newer")
        };
        QCOMPARE(
            refresh->commitArmedPublication().status,
            PublicationStatus::Conflict);
    }

    Coordinator restarted(
        QStringLiteral("athlete"),
        transactionParent,
        store.callbacks());
    const RecoveryResult recovered = restarted.recover(1000);
    QCOMPARE(recovered.status, RecoveryStatus::Recovered);
    QCOMPARE(
        store.activeValue().accessToken,
        QStringLiteral("access-newer"));
    QCOMPARE(
        store.activeValue().refreshToken,
        QStringLiteral("refresh-newer"));
    QCOMPARE(
        store.authorizationStateValue(),
        QStringLiteral("authorization_pending"));
    QVERIFY(store.remoteGrantUncertainValue());
    QVERIFY(store.pendingTransactionValue().isEmpty());

    const std::shared_ptr<Mutation> retry = restarted.begin(
        MutationKind::Authorization, 1000, error);
    QVERIFY2(retry, qPrintable(error));
    QVERIFY(retry->finishNoChange(error));
}

void TestStravaCredentialDurability::
mutationReadFailsClosedWhenCredentialsAreUnavailable()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent = createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());

    FakeSecureStore store;
    store.active = {
        QStringLiteral("access-retained"),
        QStringLiteral("refresh-retained")
    };
    Coordinator coordinator(
        QStringLiteral("athlete"),
        transactionParent,
        store.callbacks());
    QString error;
    const std::shared_ptr<Mutation> mutation = coordinator.begin(
        MutationKind::Revocation, 1000, error);
    QVERIFY2(mutation, qPrintable(error));

    store.currentReadable = false;
    StoredState stored;
    QVERIFY(!mutation->readStoredState(stored, error));
    QVERIFY(!stored.readable);
    QVERIFY(!error.isEmpty());
    QCOMPARE(store.activeValue().accessToken,
             QStringLiteral("access-retained"));
    QCOMPARE(store.activeValue().refreshToken,
             QStringLiteral("refresh-retained"));

    store.currentReadable = true;
    QVERIFY(mutation->finishNoChange(error));
}

void TestStravaCredentialDurability::
localRevocationRecoveryResumesBeforeFirstDelete()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent = createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());

    FakeSecureStore store;
    store.active = {
        QStringLiteral("access-retained"),
        QStringLiteral("refresh-retained")
    };
    Coordinator coordinator(
        QStringLiteral("athlete"),
        transactionParent,
        store.callbacks());
    QString error;
    {
        const std::shared_ptr<Mutation> mutation = coordinator.begin(
            MutationKind::Revocation, 1000, error);
        QVERIFY2(mutation, qPrintable(error));
        QVERIFY(mutation->markPendingState(error));
        QVERIFY(mutation->markLocalCommitStarted(
            QString(), PublicationMode::Authoritative, error));
        QCOMPARE(store.activeValue().accessToken,
                 QStringLiteral("access-retained"));
        QCOMPARE(store.activeValue().refreshToken,
                 QStringLiteral("refresh-retained"));
    }

    const RecoveryResult recovered = coordinator.recover(1000);
    QCOMPARE(recovered.status, RecoveryStatus::Recovered);
    QVERIFY(store.activeValue().accessToken.isEmpty());
    QVERIFY(store.activeValue().refreshToken.isEmpty());
    QCOMPARE(store.authorizationStateValue(), QStringLiteral("revoked"));
    QVERIFY(!store.remoteGrantUncertainValue());
    QCOMPARE(store.cleanupCalls, 1);
    QCOMPARE(coordinator.recover(1000).status, RecoveryStatus::NoWork);
}

void TestStravaCredentialDurability::
confirmedRemoteRevocationRecoveryResumesBeforeFirstDelete()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent = createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());

    FakeSecureStore store;
    store.active = {
        QStringLiteral("access-retained"),
        QStringLiteral("refresh-retained")
    };
    Coordinator coordinator(
        QStringLiteral("athlete"),
        transactionParent,
        store.callbacks());
    QString error;
    {
        const std::shared_ptr<Mutation> mutation = coordinator.begin(
            MutationKind::Revocation, 1000, error);
        QVERIFY2(mutation, qPrintable(error));
        QVERIFY(mutation->markPendingState(error));
        QVERIFY(mutation->markCommitUnknown(error));
        QVERIFY2(mutation->markLocalCommitStarted(
                     QStringLiteral("refresh-retained"),
                     PublicationMode::CompareAndSwap,
                     error),
                 qPrintable(error));
    }

    const RecoveryResult recovered = coordinator.recover(1000);
    QCOMPARE(recovered.status, RecoveryStatus::Recovered);
    QVERIFY(store.activeValue().accessToken.isEmpty());
    QVERIFY(store.activeValue().refreshToken.isEmpty());
    QCOMPARE(store.authorizationStateValue(), QStringLiteral("revoked"));
    QVERIFY(!store.remoteGrantUncertainValue());
}

void TestStravaCredentialDurability::coherentReadWaitsForAccountMutation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent = createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());

    FakeSecureStore store;
    store.active = {
        QStringLiteral("access-current"),
        QStringLiteral("refresh-current")
    };
    Coordinator mutator(
        QStringLiteral("athlete"), transactionParent, store.callbacks());
    Coordinator reader(
        QStringLiteral("athlete"), transactionParent, store.callbacks());
    QString error;
    const std::shared_ptr<Mutation> mutation = mutator.begin(
        MutationKind::Refresh, 1000, error);
    QVERIFY2(mutation, qPrintable(error));

    StoredState stored;
    QElapsedTimer elapsed;
    elapsed.start();
    QVERIFY(!reader.readStoredState(stored, 25, error));
    QVERIFY(!stored.readable);
    QVERIFY2(elapsed.elapsed() < 500,
             "Account snapshot read exceeded its bounded lock deadline");

    QVERIFY(mutation->finishNoChange(error));
    QVERIFY2(reader.readStoredState(stored, 1000, error),
             qPrintable(error));
    QVERIFY(stored.readable);
    QCOMPARE(stored.credentials.accessToken,
             QStringLiteral("access-current"));
    QCOMPARE(stored.credentials.refreshToken,
             QStringLiteral("refresh-current"));
}

void TestStravaCredentialDurability::coherentReadRejectsRevisionChange()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent = createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());

    FakeSecureStore store;
    store.active = {
        QStringLiteral("access-old"),
        QStringLiteral("refresh-old")
    };
    std::atomic<bool> changed{false};
    store.currentReadHook = [&store, &changed] {
        if (changed.exchange(true)) return;
        const std::lock_guard<std::mutex> lock(store.mutex);
        store.active = {
            QStringLiteral("access-new"),
            QStringLiteral("refresh-new")
        };
        store.authorizationRevision = QStringLiteral("revision-2");
    };

    Coordinator reader(
        QStringLiteral("athlete"), transactionParent, store.callbacks());
    StoredState stored;
    QString error;
    QVERIFY(!reader.readStoredState(stored, 1000, error));
    QVERIFY(!stored.readable);
    QVERIFY(!error.isEmpty());
}

void TestStravaCredentialDurability::
journalWriteDoesNotFollowReplacedNamespacePath()
{
    const TransitionActionReset resetTransition;
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent = createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());
    const QString namespacePath = QDir(transactionParent).filePath(
        QStringLiteral(".strava-credential-durability"));
    const QString displacedPath = QDir(transactionParent).filePath(
        QStringLiteral("displaced-namespace"));

    FakeSecureStore store;
    Coordinator coordinator(
        QStringLiteral("athlete"), transactionParent, store.callbacks());
    QString error;
    const std::shared_ptr<Mutation> mutation = coordinator.begin(
        MutationKind::Refresh, 1000, error);
    QVERIFY2(mutation, qPrintable(error));
    QFile original(QDir(namespacePath).filePath(QStringLiteral("state.json")));
    QVERIFY(original.open(QIODevice::ReadOnly));
    const QByteArray originalJournal = original.readAll();
    original.close();

    std::atomic<bool> swapped{false};
    setTransitionAction([&](const QString &transition) {
        if (transition != QStringLiteral("journal-before-write")
            || swapped.exchange(true)) return;
        QVERIFY(QDir().rename(namespacePath, displacedPath));
        QVERIFY(QDir().mkpath(namespacePath));
    });
    QVERIFY(!mutation->markCommitUnknown(error));
    setTransitionAction({});
    QVERIFY(swapped.load());
    QVERIFY(!QFileInfo::exists(
        QDir(namespacePath).filePath(QStringLiteral("state.json"))));

    QFile displaced(
        QDir(displacedPath).filePath(QStringLiteral("state.json")));
    QVERIFY(displaced.open(QIODevice::ReadOnly));
    QCOMPARE(displaced.readAll(), originalJournal);
}

void TestStravaCredentialDurability::
accountLockDoesNotFollowReplacedNamespacePath()
{
    const TransitionActionReset resetTransition;
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent = createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());
    const QString namespacePath = QDir(transactionParent).filePath(
        QStringLiteral(".strava-credential-durability"));
    const QString displacedPath = QDir(transactionParent).filePath(
        QStringLiteral("displaced-lock-namespace"));

    FakeSecureStore store;
    Coordinator bootstrap(
        QStringLiteral("athlete"), transactionParent, store.callbacks());
    QCOMPARE(bootstrap.recover(1000).status, RecoveryStatus::NoWork);

    std::atomic<bool> swapped{false};
    setTransitionAction([&](const QString &transition) {
        if (transition != QStringLiteral("lock-before-acquire")
            || swapped.exchange(true)) return;
        QVERIFY(QDir().rename(namespacePath, displacedPath));
        QVERIFY(QDir().mkpath(namespacePath));
    });
    Coordinator coordinator(
        QStringLiteral("athlete"), transactionParent, store.callbacks());
    QString error;
    QVERIFY(!coordinator.begin(
        MutationKind::Refresh, 1000, error));
    setTransitionAction({});
    QVERIFY(swapped.load());
    QVERIFY(!QFileInfo::exists(
        QDir(namespacePath).filePath(QStringLiteral("mutation.lock"))));
    QVERIFY(!QFileInfo::exists(
        QDir(namespacePath).filePath(QStringLiteral("state.json"))));
}

void TestStravaCredentialDurability::
transientTokenReadDoesNotRetirePublicationJournal()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent = createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());

    FakeSecureStore store;
    store.active = {
        QStringLiteral("access-old"),
        QStringLiteral("refresh-old")
    };
    store.failAccessWrites = 1;
    Coordinator coordinator(
        QStringLiteral("athlete"),
        transactionParent,
        store.callbacks());
    QString error;
    std::shared_ptr<Mutation> mutation = coordinator.begin(
        MutationKind::Refresh, 1000, error);
    QVERIFY2(mutation, qPrintable(error));
    QCOMPARE(
        mutation->publish(replacementPublication(QStringLiteral("new"))).status,
        PublicationStatus::Pending);
    mutation.reset();

    store.currentReadable = false;
    const RecoveryResult unavailable = coordinator.recover(1000);
    QCOMPARE(unavailable.status, RecoveryStatus::StorageFailure);
    QVERIFY(!store.pendingTransactionValue().isEmpty());
    QCOMPARE(
        store.authorizationStateValue(),
        QStringLiteral("authorization_pending"));

    store.currentReadable = true;
    const RecoveryResult recovered = coordinator.recover(1000);
    QCOMPARE(recovered.status, RecoveryStatus::Recovered);
    QCOMPARE(store.activeValue().accessToken, QStringLiteral("access-new"));
    QVERIFY(store.pendingTransactionValue().isEmpty());
}

void TestStravaCredentialDurability::
transientPendingPackageReadDoesNotRetireJournal()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent = createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());

    FakeSecureStore store;
    store.active = {
        QStringLiteral("access-old"),
        QStringLiteral("refresh-old")
    };
    store.failTimestampWrites = 1;
    Coordinator coordinator(
        QStringLiteral("athlete"),
        transactionParent,
        store.callbacks());
    QString error;
    std::shared_ptr<Mutation> mutation = coordinator.begin(
        MutationKind::Refresh, 1000, error);
    QVERIFY2(mutation, qPrintable(error));
    QCOMPARE(
        mutation->publish(replacementPublication(QStringLiteral("new"))).status,
        PublicationStatus::Pending);
    const QString pendingPackage = store.pendingTransactionValue();
    QVERIFY(!pendingPackage.isEmpty());
    mutation.reset();

    store.pendingTransactionReadable = false;
    const RecoveryResult unavailable = coordinator.recover(1000);
    QCOMPARE(unavailable.status, RecoveryStatus::StorageFailure);
    QCOMPARE(store.pendingTransactionValue(), pendingPackage);

    store.pendingTransactionReadable = true;
    const RecoveryResult recovered = coordinator.recover(1000);
    QCOMPARE(recovered.status, RecoveryStatus::Recovered);
    QVERIFY(store.pendingTransactionValue().isEmpty());
}

void TestStravaCredentialDurability::
transientRevocationReadDoesNotRetireJournal()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent = createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());

    FakeSecureStore store;
    store.active = {
        QStringLiteral("access-old"),
        QStringLiteral("refresh-old")
    };
    Coordinator coordinator(
        QStringLiteral("athlete"),
        transactionParent,
        store.callbacks());
    QString error;
    {
        const std::shared_ptr<Mutation> mutation = coordinator.begin(
            MutationKind::Revocation, 1000, error);
        QVERIFY2(mutation, qPrintable(error));
        QVERIFY(mutation->markPendingState(error));
        QVERIFY(mutation->markCommitUnknown(error));
        const StorageCallbacks storage = store.callbacks();
        QVERIFY(storage.writeAccessToken(QString()));
        QVERIFY(storage.writeRefreshToken(QString()));
    }

    store.currentReadable = false;
    const RecoveryResult unavailable = coordinator.recover(1000);
    QCOMPARE(unavailable.status, RecoveryStatus::StorageFailure);
    QCOMPARE(store.cleanupCalls, 0);
    QCOMPARE(
        store.authorizationStateValue(),
        QStringLiteral("revocation_pending"));

    store.currentReadable = true;
    const RecoveryResult recovered = coordinator.recover(1000);
    QCOMPARE(recovered.status, RecoveryStatus::Recovered);
    QCOMPARE(store.cleanupCalls, 1);
    QCOMPARE(
        store.authorizationStateValue(),
        QStringLiteral("revoked"));
}

void TestStravaCredentialDurability::
partialPublicationRecoversAfterRestart_data()
{
    QTest::addColumn<int>("accessFailures");
    QTest::addColumn<int>("timestampFailures");
    QTest::newRow("access-token") << 1 << 0;
    QTest::newRow("timestamp") << 0 << 1;
}

void TestStravaCredentialDurability::
partialPublicationRecoversAfterRestart()
{
    QFETCH(int, accessFailures);
    QFETCH(int, timestampFailures);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent =
        createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());

    FakeSecureStore store;
    store.active = {
        QStringLiteral("access-old"),
        QStringLiteral("refresh-old")
    };
    store.timestamp = QStringLiteral("timestamp-old");
    store.failAccessWrites = accessFailures;
    store.failTimestampWrites = timestampFailures;

    {
        Coordinator first(
            QStringLiteral("athlete"),
            transactionParent,
            store.callbacks());
        QString error;
        const std::shared_ptr<Mutation> mutation = first.begin(
            MutationKind::Refresh, 1000, error);
        QVERIFY2(mutation, qPrintable(error));

        const PublicationResult result = mutation->publish(
            replacementPublication(QStringLiteral("new")));
        QCOMPARE(result.status, PublicationStatus::Pending);
        QCOMPARE(
            store.authorizationStateValue(),
            QStringLiteral("authorization_pending"));
        QVERIFY(!store.pendingTransactionValue().isEmpty());
        QFile journal(QDir(transactionParent).filePath(
            QStringLiteral(
                ".strava-credential-durability/state.json")));
        QVERIFY(journal.open(QIODevice::ReadOnly));
        const QByteArray journalContents = journal.readAll();
        QVERIFY(!journalContents.contains("access-old"));
        QVERIFY(!journalContents.contains("refresh-old"));
        QVERIFY(!journalContents.contains("access-new"));
        QVERIFY(!journalContents.contains("refresh-new"));
        journal.close();
        QVERIFY(store.activeValue().accessToken
                != QStringLiteral("access-new")
            || store.timestampValue()
                != QStringLiteral(
                    "2026-08-05T12:00:00.000+03:00"));
    }

    Coordinator restarted(
        QStringLiteral("athlete"),
        transactionParent,
        store.callbacks());
    const RecoveryResult recovered = restarted.recover(1000);
    QCOMPARE(recovered.status, RecoveryStatus::Recovered);
    QCOMPARE(store.activeValue().accessToken, QStringLiteral("access-new"));
    QCOMPARE(store.activeValue().refreshToken, QStringLiteral("refresh-new"));
    QCOMPARE(
        store.timestampValue(),
        QStringLiteral("2026-08-05T12:00:00.000+03:00"));
    QCOMPARE(store.authorizationStateValue(), QStringLiteral("active"));
    QVERIFY(!store.remoteGrantUncertainValue());
    QVERIFY(store.pendingTransactionValue().isEmpty());

    const RecoveryResult idempotent = restarted.recover(1000);
    QCOMPARE(idempotent.status, RecoveryStatus::NoWork);
}

void TestStravaCredentialDurability::
processDeathDuringPublicationRecoversDurably()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent =
        createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());

    const QString vaultPath =
        temporary.filePath(QStringLiteral("vault.ini"));
    const QString providerPath =
        temporary.filePath(QStringLiteral("provider.ini"));
    const SharedDurableStore vault(vaultPath);
    const SharedDurableStore provider(providerPath);
    QVERIFY(vault.write(QStringLiteral("access"),
                        QStringLiteral("access-old")));
    QVERIFY(vault.write(QStringLiteral("refresh"),
                        QStringLiteral("refresh-old")));
    QVERIFY(vault.write(QStringLiteral("timestamp"),
                        QStringLiteral("timestamp-old")));
    QVERIFY(vault.write(QStringLiteral("authorization_state"),
                        QStringLiteral("active")));
    QVERIFY(vault.write(QStringLiteral("uncertain"),
                        QStringLiteral("false")));
    QVERIFY(provider.write(QStringLiteral("refresh"),
                           QStringLiteral("refresh-old")));

    const QString resultPath =
        temporary.filePath(QStringLiteral("crash-result.txt"));
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(), {
        QStringLiteral("--grant-child"),
        transactionParent,
        QStringLiteral("athlete"),
        vaultPath,
        providerPath,
        QStringLiteral("crash-refresh"),
        QStringLiteral("0"),
        resultPath
    });
    QVERIFY(child.waitForStarted(2000));
    QVERIFY(child.waitForFinished(5000));
    QCOMPARE(child.exitCode(), 86);
    QVERIFY(!QFileInfo::exists(resultPath));
    QCOMPARE(vault.read(QStringLiteral("refresh")),
             QStringLiteral("refresh-new"));
    QCOMPARE(vault.read(QStringLiteral("access")),
             QStringLiteral("access-old"));

    Coordinator restarted(
        QStringLiteral("athlete"),
        transactionParent,
        vault.callbacks());
    const RecoveryResult recovered = restarted.recover(1000);
    QCOMPARE(recovered.status, RecoveryStatus::Recovered);
    QCOMPARE(vault.read(QStringLiteral("access")),
             QStringLiteral("access-new"));
    QCOMPARE(vault.read(QStringLiteral("refresh")),
             QStringLiteral("refresh-new"));
    QCOMPARE(vault.read(QStringLiteral("timestamp")),
             QStringLiteral(
                 "2026-08-05T12:00:00.000+03:00"));
    QCOMPARE(vault.read(QStringLiteral("authorization_state")),
             QStringLiteral("active"));
    QCOMPARE(vault.read(QStringLiteral("uncertain")),
             QStringLiteral("false"));
    QCOMPARE(restarted.recover(1000).status,
             RecoveryStatus::NoWork);
}

void TestStravaCredentialDurability::
recoveryDistinguishesUnstartedAndUnknownRemoteMutation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent =
        createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());

    FakeSecureStore store;
    store.active = {
        QStringLiteral("access-old"),
        QStringLiteral("refresh-old")
    };
    Coordinator coordinator(
        QStringLiteral("athlete"),
        transactionParent,
        store.callbacks());

    QString error;
    {
        const std::shared_ptr<Mutation> unstarted = coordinator.begin(
            MutationKind::Refresh, 1000, error);
        QVERIFY2(unstarted, qPrintable(error));
        QVERIFY(unstarted->markPendingState(error));
        QCOMPARE(store.authorizationStateValue(),
                 QStringLiteral("authorization_pending"));
        QVERIFY(store.remoteGrantUncertainValue());
    }
    const RecoveryResult unstartedRecovery = coordinator.recover(1000);
    QCOMPARE(unstartedRecovery.status, RecoveryStatus::Recovered);
    QCOMPARE(
        store.authorizationStateValue(),
        QStringLiteral("active"));
    QVERIFY(!store.remoteGrantUncertainValue());

    {
        const std::shared_ptr<Mutation> unknown = coordinator.begin(
            MutationKind::Authorization, 1000, error);
        QVERIFY2(unknown, qPrintable(error));
        QVERIFY(unknown->markCommitUnknown(error));
    }
    const RecoveryResult unknownRecovery = coordinator.recover(1000);
    QCOMPARE(unknownRecovery.status, RecoveryStatus::Pending);
    QCOMPARE(
        coordinator.recover(1000).status,
        RecoveryStatus::NoWork);
    const std::shared_ptr<Mutation> reauthorization = coordinator.begin(
        MutationKind::Authorization, 1000, error);
    QVERIFY2(reauthorization, qPrintable(error));
    QCOMPARE(reauthorization->generation(), quint64(3));
    QVERIFY(reauthorization->finishNoChange(error));
}

void TestStravaCredentialDurability::
unknownRevocationRecoveryPreservesVaultGrantAndAllowsCleanup()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent =
        createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());

    FakeSecureStore store;
    store.active = {
        QStringLiteral("access-old"),
        QStringLiteral("refresh-old")
    };
    QString error;
    {
        Coordinator coordinator(
            QStringLiteral("athlete"),
            transactionParent,
            store.callbacks());
        const std::shared_ptr<Mutation> removal = coordinator.begin(
            MutationKind::Revocation, 1000, error);
        QVERIFY2(removal, qPrintable(error));
        QVERIFY(removal->markPendingState(error));
        QVERIFY(removal->markCommitUnknown(error));
        const StorageCallbacks storage = store.callbacks();
        QVERIFY(storage.writeAccessToken(
            QStringLiteral("access-new")));
        QVERIFY(storage.writeRefreshToken(
            QStringLiteral("refresh-new")));
    }

    Coordinator restarted(
        QStringLiteral("athlete"),
        transactionParent,
        store.callbacks());
    const RecoveryResult recovered = restarted.recover(1000);
    QCOMPARE(recovered.status, RecoveryStatus::Pending);
    QCOMPARE(store.activeValue().accessToken,
             QStringLiteral("access-new"));
    QCOMPARE(store.activeValue().refreshToken,
             QStringLiteral("refresh-new"));
    QCOMPARE(store.authorizationStateValue(),
             QStringLiteral("revocation_pending"));
    QVERIFY(store.remoteGrantUncertainValue());
    QCOMPARE(restarted.recover(1000).status, RecoveryStatus::NoWork);
    StoredState pending;
    QVERIFY2(
        restarted.readStoredState(pending, 1000, error),
        qPrintable(error));
    QVERIFY(pending.readable);
    QCOMPARE(pending.authorizationState,
             QStringLiteral("revocation_pending"));
    QVERIFY(pending.remoteGrantUncertain);
    QCOMPARE(pending.credentials.accessToken,
             QStringLiteral("access-new"));
    QCOMPARE(pending.credentials.refreshToken,
             QStringLiteral("refresh-new"));

    {
        const std::shared_ptr<Mutation> cleanup = restarted.begin(
            MutationKind::Revocation, 1000, error);
        QVERIFY2(cleanup, qPrintable(error));
        QVERIFY(cleanup->markPendingState(error));
        QVERIFY(cleanup->markLocalCommitStarted(
            QString(), PublicationMode::Authoritative, error));
    }

    Coordinator afterCleanupRestart(
        QStringLiteral("athlete"),
        transactionParent,
        store.callbacks());
    const RecoveryResult cleaned =
        afterCleanupRestart.recover(1000);
    QCOMPARE(cleaned.status, RecoveryStatus::Recovered);
    QVERIFY(store.activeValue().accessToken.isEmpty());
    QVERIFY(store.activeValue().refreshToken.isEmpty());
    QCOMPARE(store.authorizationStateValue(),
             QStringLiteral("revoked"));
    QVERIFY(!store.remoteGrantUncertainValue());
    QCOMPARE(afterCleanupRestart.recover(1000).status,
             RecoveryStatus::NoWork);
}

void TestStravaCredentialDurability::
legacyRemotePendingJournalMigratesFailClosed()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent =
        createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());

    FakeSecureStore store;
    store.active = {
        QStringLiteral("access-old"),
        QStringLiteral("refresh-old")
    };
    Coordinator coordinator(
        QStringLiteral("athlete"),
        transactionParent,
        store.callbacks());
    QCOMPARE(coordinator.recover(1000).status, RecoveryStatus::NoWork);

    QJsonObject legacy;
    legacy.insert(QStringLiteral("version"), 1);
    legacy.insert(
        QStringLiteral("account_sha256"),
        QString::fromLatin1(
            QCryptographicHash::hash(
                QByteArrayLiteral("athlete"),
                QCryptographicHash::Sha256).toHex()));
    legacy.insert(QStringLiteral("generation"), QStringLiteral("4"));
    legacy.insert(QStringLiteral("phase"),
                  QStringLiteral("remote_pending"));
    legacy.insert(
        QStringLiteral("transaction_id"),
        QStringLiteral("01234567-89ab-cdef-0123-456789abcdef"));
    legacy.insert(QStringLiteral("kind"), QStringLiteral("refresh"));
    QFile journal(QDir(transactionParent).filePath(
        QStringLiteral(".strava-credential-durability/state.json")));
    QVERIFY(journal.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray encoded =
        QJsonDocument(legacy).toJson(QJsonDocument::Compact);
    QCOMPARE(journal.write(encoded), qint64(encoded.size()));
    journal.close();
#ifndef Q_OS_WIN
    QVERIFY(QFile::setPermissions(
        journal.fileName(),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner));
#endif

    const RecoveryResult migrated = coordinator.recover(1000);
    QCOMPARE(migrated.status, RecoveryStatus::Pending);
    QCOMPARE(migrated.generation, quint64(4));
    QCOMPARE(store.authorizationStateValue(),
             QStringLiteral("authorization_pending"));
    QVERIFY(store.remoteGrantUncertainValue());
    QCOMPARE(coordinator.recover(1000).status, RecoveryStatus::NoWork);

    QString error;
    const std::shared_ptr<Mutation> next = coordinator.begin(
        MutationKind::Refresh, 1000, error);
    QVERIFY2(next, qPrintable(error));
    QCOMPARE(next->generation(), quint64(5));
    QVERIFY(next->finishNoChange(error));
}

void TestStravaCredentialDurability::
independentProcessesSerializeAndFenceGenerations()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent =
        createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());

    const QString vaultPath =
        temporary.filePath(QStringLiteral("vault.ini"));
    const QString providerPath =
        temporary.filePath(QStringLiteral("provider.ini"));
    const SharedDurableStore vault(vaultPath);
    const SharedDurableStore provider(providerPath);
    QVERIFY(vault.write(QStringLiteral("access"),
                        QStringLiteral("access-old")));
    QVERIFY(vault.write(QStringLiteral("refresh"),
                        QStringLiteral("refresh-old")));
    QVERIFY(vault.write(QStringLiteral("timestamp"),
                        QStringLiteral("timestamp-old")));
    QVERIFY(vault.write(QStringLiteral("authorization_state"),
                        QStringLiteral("active")));
    QVERIFY(vault.write(QStringLiteral("uncertain"),
                        QStringLiteral("false")));
    QVERIFY(provider.write(QStringLiteral("refresh"),
                           QStringLiteral("refresh-old")));

    const QString refreshResult =
        temporary.filePath(QStringLiteral("refresh.txt"));
    const QString removalResult =
        temporary.filePath(QStringLiteral("removal.txt"));
    const QString oauthResult =
        temporary.filePath(QStringLiteral("oauth.txt"));
    const QString program = QCoreApplication::applicationFilePath();

    QProcess refresh;
    refresh.start(program, {
        QStringLiteral("--grant-child"),
        transactionParent,
        QStringLiteral("athlete"),
        vaultPath,
        providerPath,
        QStringLiteral("refresh"),
        QStringLiteral("350"),
        refreshResult
    });
    QVERIFY(refresh.waitForStarted(2000));
    QTRY_VERIFY_WITH_TIMEOUT(
        QFileInfo::exists(refreshResult + QStringLiteral(".acquired")),
        2000);

    QProcess removal;
    removal.start(program, {
        QStringLiteral("--grant-child"),
        transactionParent,
        QStringLiteral("athlete"),
        vaultPath,
        providerPath,
        QStringLiteral("remove"),
        QStringLiteral("350"),
        removalResult
    });
    QVERIFY(removal.waitForStarted(2000));
    QTRY_VERIFY_WITH_TIMEOUT(
        QFileInfo::exists(removalResult + QStringLiteral(".acquired")),
        5000);

    QProcess oauth;
    oauth.start(program, {
        QStringLiteral("--grant-child"),
        transactionParent,
        QStringLiteral("athlete"),
        vaultPath,
        providerPath,
        QStringLiteral("oauth"),
        QStringLiteral("0"),
        oauthResult
    });
    QVERIFY(oauth.waitForStarted(2000));

    const auto waitForFinished = [](QProcess &process) {
        return process.state() == QProcess::NotRunning
            || process.waitForFinished(5000);
    };
    QVERIFY(waitForFinished(refresh));
    QVERIFY(waitForFinished(removal));
    QVERIFY(waitForFinished(oauth));
    QCOMPARE(refresh.exitCode(), 0);
    QCOMPARE(removal.exitCode(), 0);
    QCOMPARE(oauth.exitCode(), 0);

    const auto readResult = [](const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return QList<QByteArray>();
        return file.readAll().trimmed().split(' ');
    };
    const QList<QByteArray> refreshParts = readResult(refreshResult);
    const QList<QByteArray> removalParts = readResult(removalResult);
    const QList<QByteArray> oauthParts = readResult(oauthResult);
    QCOMPARE(refreshParts.size(), 2);
    QCOMPARE(removalParts.size(), 2);
    QCOMPARE(oauthParts.size(), 2);
    QCOMPARE(refreshParts.at(0).toULongLong(), quint64(1));
    QCOMPARE(removalParts.at(0).toULongLong(), quint64(2));
    QCOMPARE(oauthParts.at(0).toULongLong(), quint64(3));
    QCOMPARE(refreshParts.at(1), QByteArrayLiteral("refresh-old"));
    QCOMPARE(removalParts.at(1), QByteArrayLiteral("refresh-new"));
    QCOMPARE(oauthParts.at(1), QByteArrayLiteral("-"));
    QCOMPARE(vault.read(QStringLiteral("refresh")),
             QStringLiteral("refresh-oauth"));
    QCOMPARE(provider.read(QStringLiteral("refresh")),
             QStringLiteral("refresh-oauth"));
    QFile journal(QDir(transactionParent).filePath(
        QStringLiteral(".strava-credential-durability/state.json")));
    QVERIFY(journal.open(QIODevice::ReadOnly));
    const QByteArray journalContents = journal.readAll();
    QVERIFY(!journalContents.contains("refresh-old"));
    QVERIFY(!journalContents.contains("refresh-new"));
    QVERIFY(!journalContents.contains("refresh-oauth"));
}

void TestStravaCredentialDurability::
guiOriginatedCredentialWorkRunsOffApplicationThread()
{
    std::atomic<bool> ran{false};
    std::atomic<bool> ranOnApplicationThread{true};
    const StravaSettingsCommit::DispatchResult dispatched =
        StravaSettingsCommit::runOnCredentialThread(
            [&] {
                ran.store(true);
                ranOnApplicationThread.store(
                    QThread::currentThread()
                    == QCoreApplication::instance()->thread());
            },
            1000,
            {});

    QCOMPARE(
        dispatched.status,
        StravaSettingsCommit::DispatchStatus::Completed);
    QVERIFY(ran.load());
    QVERIFY(!ranOnApplicationThread.load());
}

void TestStravaCredentialDurability::
startedCredentialCommitReturnsTrackedPendingWithinDeadline()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent =
        createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());

    FakeSecureStore store;
    store.active = {
        QStringLiteral("access-old"),
        QStringLiteral("refresh-old")
    };
    Coordinator coordinator(
        QStringLiteral("athlete"),
        transactionParent,
        store.callbacks());
    QString error;
    const std::shared_ptr<Mutation> mutation = coordinator.begin(
        MutationKind::Refresh, 1000, error);
    QVERIFY2(mutation, qPrintable(error));
    QVERIFY(mutation->armPublication(
        replacementPublication(QStringLiteral("blocked")), error));

    std::atomic<bool> backendStarted{false};
    std::atomic<bool> backendRanOnApplicationThread{false};
    store.refreshWriteHook = [
        &backendStarted, &backendRanOnApplicationThread] {
        backendStarted.store(true);
        backendRanOnApplicationThread.store(
            QThread::currentThread()
            == QCoreApplication::instance()->thread());
        QThread::msleep(250);
    };
    std::atomic<qint64> callerElapsed{0};
    std::future<StravaSettingsCommit::DispatchStatus> caller =
        std::async(std::launch::async, [&] {
            QElapsedTimer elapsed;
            elapsed.start();
            const StravaSettingsCommit::DispatchResult dispatched =
                StravaSettingsCommit::runOnCredentialThread(
                    [mutation] {
                        mutation->commitArmedPublication();
                    },
                    50,
                    {});
            callerElapsed.store(elapsed.elapsed());
            return dispatched.status;
        });

    QTRY_VERIFY_WITH_TIMEOUT(backendStarted.load(), 1000);
    QCOMPARE(
        caller.wait_for(std::chrono::milliseconds(200)),
        std::future_status::ready);
    QCOMPARE(
        caller.get(),
        StravaSettingsCommit::DispatchStatus::Pending);
    QVERIFY2(callerElapsed.load() < 200,
             "A started settings commit blocked its caller teardown");
    QVERIFY(!backendRanOnApplicationThread.load());

    Coordinator contender(
        QStringLiteral("athlete"),
        transactionParent,
        store.callbacks());
    QString contenderError;
    const std::shared_ptr<Mutation> superseding = contender.begin(
        MutationKind::Authorization, 50, contenderError, false);
    QVERIFY2(!superseding,
             "A late credential commit released its generation lease");

    const RecoveryResult completed = coordinator.recover(1000);
    QCOMPARE(completed.status, RecoveryStatus::NoWork);
    QCOMPARE(
        store.activeValue().accessToken,
        QStringLiteral("access-blocked"));
    QCOMPARE(store.authorizationStateValue(), QStringLiteral("active"));
}

void TestStravaCredentialDurability::
latePendingStateWriteCannotCrossMutationGeneration()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent =
        createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());

    FakeSecureStore store;
    Coordinator coordinator(
        QStringLiteral("athlete"),
        transactionParent,
        store.callbacks());
    QString error;
    const std::shared_ptr<Mutation> first = coordinator.begin(
        MutationKind::Refresh, 1000, error);
    QVERIFY2(first, qPrintable(error));

    std::promise<void> releaseWrite;
    const std::shared_future<void> writeReleased =
        releaseWrite.get_future().share();
    std::atomic<bool> writeStarted{false};
    store.setStateWriteHook([&writeStarted, writeReleased] {
        writeStarted.store(true);
        writeReleased.wait();
    });
    const StravaSettingsCommit::DispatchResult dispatched =
        StravaSettingsCommit::runOnCredentialThread(
            [first] {
                QString stateError;
                first->markPendingState(stateError);
            },
            50,
            {});
    QCOMPARE(dispatched.status,
             StravaSettingsCommit::DispatchStatus::Pending);
    QVERIFY(writeStarted.load());

    std::future<bool> retired = std::async(
        std::launch::async,
        [first] {
            QString finishError;
            return first->finishNoChange(finishError);
        });
    QCOMPARE(retired.wait_for(std::chrono::milliseconds(50)),
             std::future_status::timeout);

    releaseWrite.set_value();
    QVERIFY(retired.get());
    store.setStateWriteHook({});

    const std::shared_ptr<Mutation> second = coordinator.begin(
        MutationKind::Authorization, 1000, error);
    QVERIFY2(second, qPrintable(error));
    QCOMPARE(second->generation(), quint64(2));
    QVERIFY(second->markPendingState(error));
    QVERIFY(second->finishNoChange(error));
    QCOMPARE(store.authorizationStateValue(), QStringLiteral("active"));
    QVERIFY(!store.remoteGrantUncertainValue());
}

void TestStravaCredentialDurability::
leaseRefreshesMetadataBeforeCapturingRollbackState()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent =
        createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());

    FakeSecureStore store;
    store.authorizationState = QStringLiteral("revocation_pending");
    store.remoteGrantUncertain = true;
    store.metadataRefreshHook = [&store] {
        const std::lock_guard<std::mutex> lock(store.mutex);
        store.authorizationState = QStringLiteral("active");
        store.remoteGrantUncertain = false;
    };
    Coordinator coordinator(
        QStringLiteral("athlete"),
        transactionParent,
        store.callbacks());

    QString error;
    const std::shared_ptr<Mutation> mutation = coordinator.begin(
        MutationKind::Refresh, 1000, error);
    QVERIFY2(mutation, qPrintable(error));
    QVERIFY(mutation->markPendingState(error));
    QCOMPARE(store.authorizationStateValue(),
             QStringLiteral("authorization_pending"));
    QVERIFY(mutation->finishNoChange(error));
    QCOMPARE(store.authorizationStateValue(), QStringLiteral("active"));
    QVERIFY(!store.remoteGrantUncertainValue());
    QCOMPARE(store.refreshCalls, 1);
}

void TestStravaCredentialDurability::
revocationCleanupRemainsRecoverableUntilDurable()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent =
        createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());

    FakeSecureStore store;
    store.authorizationState = QStringLiteral("active");
    store.remoteGrantUncertain = false;
    store.failCleanupWrites = 1;
    Coordinator coordinator(
        QStringLiteral("athlete"),
        transactionParent,
        store.callbacks());

    QString error;
    {
        const std::shared_ptr<Mutation> mutation = coordinator.begin(
            MutationKind::Revocation, 1000, error);
        QVERIFY2(mutation, qPrintable(error));
        QVERIFY(mutation->markPendingState(error));
        QVERIFY(mutation->markCommitUnknown(error));
    }

    const RecoveryResult first = coordinator.recover(1000);
    QCOMPARE(first.status, RecoveryStatus::Pending);
    QCOMPARE(store.cleanupCalls, 1);
    QCOMPARE(store.authorizationStateValue(),
             QStringLiteral("revocation_pending"));

    const RecoveryResult second = coordinator.recover(1000);
    QCOMPARE(second.status, RecoveryStatus::Recovered);
    QCOMPARE(store.cleanupCalls, 2);
    QCOMPARE(store.authorizationStateValue(), QStringLiteral("revoked"));
    QVERIFY(!store.remoteGrantUncertainValue());
    QCOMPARE(coordinator.recover(1000).status, RecoveryStatus::NoWork);
}

void TestStravaCredentialDurability::
unstartedRemoteCommitRestoresPreviousAuthorization()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent =
        createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());

    FakeSecureStore store;
    store.active = {
        QStringLiteral("access-old"),
        QStringLiteral("refresh-old")
    };
    Coordinator coordinator(
        QStringLiteral("athlete"),
        transactionParent,
        store.callbacks());

    QString error;
    const std::shared_ptr<Mutation> mutation = coordinator.begin(
        MutationKind::Authorization, 1000, error);
    QVERIFY2(mutation, qPrintable(error));
    QVERIFY(mutation->markPendingState(error));
    QVERIFY(mutation->markCommitUnknown(error));
    QVERIFY(mutation->abortBeforeRemoteDispatch(error));
    QCOMPARE(store.authorizationStateValue(), QStringLiteral("active"));
    QVERIFY(!store.remoteGrantUncertainValue());
    QCOMPARE(coordinator.recover(1000).status, RecoveryStatus::NoWork);
}

void TestStravaCredentialDurability::
authorizationTransitionsPublishRevisionEvidence()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString transactionParent =
        createTransactionParent(temporary);
    QVERIFY(!transactionParent.isEmpty());

    FakeSecureStore store;
    Coordinator coordinator(
        QStringLiteral("athlete"),
        transactionParent,
        store.callbacks());
    QString error;
    const std::shared_ptr<Mutation> mutation = coordinator.begin(
        MutationKind::Authorization, 1000, error);
    QVERIFY2(mutation, qPrintable(error));

    QCOMPARE(store.revisionTouches, 0);
    QVERIFY(mutation->markPendingState(error));
    QCOMPARE(store.revisionTouches, 1);
    QVERIFY(mutation->markCommitUnknown(error));
    QCOMPARE(store.revisionTouches, 1);
    QVERIFY(mutation->abortBeforeRemoteDispatch(error));
    QCOMPARE(store.revisionTouches, 2);
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() > 1
        && arguments.at(1) == QStringLiteral("--grant-child")) {
        return runGrantChild(arguments);
    }

    TestStravaCredentialDurability test;
    return QTest::qExec(&test, argc, argv);
}

#include "testStravaCredentialDurability.moc"
