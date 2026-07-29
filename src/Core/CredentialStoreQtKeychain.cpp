#include "CredentialStoreQtKeychain.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMetaObject>
#include <QMutex>
#include <QThread>
#include <QTimer>

#if defined(HAVE_LIBSECRET)
#include <qtkeychain/libsecret_p.h>
#endif

#include <memory>
#include <utility>

namespace {

const QString credentialService =
    QStringLiteral("org.goldencheetah.GoldenCheetah.credentials");
constexpr int credentialJobTimeoutMs = 15000;

enum class CredentialJobAccess
{
    ReadOnly,
    Mutating
};

struct CredentialJobGate
{
    quint64 activeOwner = 0;
    quint64 nextOwner = 0;
    std::unique_ptr<QLockFile> mutationLock;
    QString mutationLockPath;
};

struct CredentialJobGateAcquisition
{
    quint64 owner = 0;
    QString error;
};

CredentialJobGate &credentialJobGate()
{
    static CredentialJobGate gate;
    return gate;
}

CredentialJobGateAcquisition acquireCredentialJobGate(
    CredentialJobAccess access,
    const QString &mutationLockPath)
{
    CredentialJobGate &gate = credentialJobGate();
    if (gate.activeOwner != 0) {
        return {
            0,
            QStringLiteral(
                "Another credential store operation is still active")
        };
    }

    std::unique_ptr<QLockFile> mutationLock;
    if (access == CredentialJobAccess::Mutating
        && !mutationLockPath.isEmpty()) {
        mutationLock =
            std::make_unique<QLockFile>(
                mutationLockPath);
        mutationLock->setStaleLockTime(0);
        if (!mutationLock->tryLock(0)) {
            return {
                0,
                QStringLiteral(
                    "Another credential mutation is still active")
            };
        }
        if (CredentialSettingsDetail::
                backendMutationMarkerStatus(
                    mutationLockPath)
            != CredentialSettingsDetail::
                BackendMutationMarkerStatus::Absent) {
            return {
                0,
                QStringLiteral(
                    "A previous credential mutation "
                    "has an unresolved outcome")
            };
        }
        if (!CredentialSettingsDetail::
                createBackendMutationMarker(
                    mutationLockPath)) {
            return {
                0,
                QStringLiteral(
                    "Cannot persist credential mutation state")
            };
        }
    }

    do {
        ++gate.nextOwner;
    } while (gate.nextOwner == 0);
    gate.activeOwner = gate.nextOwner;
    gate.mutationLock = std::move(mutationLock);
    gate.mutationLockPath = mutationLockPath;
    return {gate.activeOwner, QString()};
}

QString releaseCredentialJobGate(quint64 owner)
{
    CredentialJobGate &gate = credentialJobGate();
    if (gate.activeOwner == owner) {
        if (!gate.mutationLockPath.isEmpty()
            && !CredentialSettingsDetail::
                removeBackendMutationMarker(
                    gate.mutationLockPath)) {
            return QStringLiteral(
                "Cannot clear credential mutation state");
        }
        gate.mutationLock.reset();
        gate.mutationLockPath.clear();
        gate.activeOwner = 0;
    }
    return QString();
}

#ifdef GC_CREDENTIAL_TEST_HOOKS
struct CredentialJobTestHooks
{
    CredentialStoreQtKeychainDetail::JobStartHook startHook;
    CredentialStoreQtKeychainDetail::JobTimeoutHook timeoutHook;
    int timeoutMs = credentialJobTimeoutMs;
};

CredentialJobTestHooks &credentialJobTestHooks()
{
    static CredentialJobTestHooks hooks;
    return hooks;
}
#endif

int configuredCredentialJobTimeoutMs()
{
#ifdef GC_CREDENTIAL_TEST_HOOKS
    return credentialJobTestHooks().timeoutMs;
#else
    return credentialJobTimeoutMs;
#endif
}

void startCredentialJob(QKeychain::Job *job)
{
#ifdef GC_CREDENTIAL_TEST_HOOKS
    const auto &hook = credentialJobTestHooks().startHook;
    if (hook && hook(job))
        return;
#endif
    job->start();
}

struct JobResult
{
    QKeychain::Error error = QKeychain::OtherError;
    QString value;
    QString errorString;
    bool indeterminate = false;
};

JobResult unavailableResult(
    const QString &error)
{
    return {
        QKeychain::NoBackendAvailable,
        QString(),
        error,
        false
    };
}

template<typename Job, typename Configure, typename Extract>
JobResult executeJobOnCurrentThread(const QString &key,
                                    CredentialJobAccess access,
                                    const QString &mutationLockPath,
                                    Configure configure,
                                    Extract extract)
{
    QCoreApplication *application =
        QCoreApplication::instance();
    if (!application || QCoreApplication::closingDown()) {
        return unavailableResult(
            QStringLiteral(
                "No running application event loop"));
    }

    const CredentialJobGateAcquisition gate =
        acquireCredentialJobGate(
            access, mutationLockPath);
    if (gate.owner == 0)
        return unavailableResult(gate.error);
    const quint64 gateOwner = gate.owner;

    auto job = std::make_unique<Job>(credentialService);
    CredentialStoreQtKeychainDetail::configureJob(job.get(), key);
    configure(job.get());

    const auto terminalReleaseError =
        std::make_shared<QString>();
    QObject::connect(
        job.get(), &QKeychain::Job::finished,
        application, [
            gateOwner,
            terminalReleaseError](QKeychain::Job *) {
            *terminalReleaseError =
                releaseCredentialJobGate(gateOwner);
        });
    QObject::connect(
        job.get(), &QObject::destroyed,
        application, [gateOwner, access](QObject *) {
            if (access == CredentialJobAccess::ReadOnly)
                releaseCredentialJobGate(gateOwner);
        });

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool finished = false;
    bool timedOut = false;
    QObject::connect(job.get(), &QKeychain::Job::finished,
                     &loop, [&finished, &loop](QKeychain::Job *) {
        finished = true;
        loop.quit();
    });
    QObject::connect(&timeout, &QTimer::timeout,
                     &loop, [
                         &timedOut, &loop,
                         jobPointer = job.get()]() {
        timedOut = true;
#ifdef GC_CREDENTIAL_TEST_HOOKS
        const auto &hook =
            credentialJobTestHooks().timeoutHook;
        if (hook)
            hook(jobPointer);
#endif
        loop.quit();
    });

    timeout.start(configuredCredentialJobTimeoutMs());
    startCredentialJob(job.get());
    if (!finished) loop.exec();

    if (!finished) {
        Q_ASSERT(timedOut || QCoreApplication::closingDown());
        job->setAutoDelete(true);
        job.release();
        return {
            QKeychain::NoBackendAvailable,
            QString(),
            timedOut
                ? QStringLiteral(
                      "Credential store operation timed out "
                      "and may still complete")
                : QStringLiteral(
                      "Credential store operation was interrupted "
                      "and may still complete"),
            access == CredentialJobAccess::Mutating
        };
    }

    timeout.stop();
    if (!terminalReleaseError->isEmpty()) {
        return {
            QKeychain::NoBackendAvailable,
            QString(),
            *terminalReleaseError,
            access == CredentialJobAccess::Mutating
        };
    }
    return {
        job->error(),
        extract(job.get()),
        job->errorString(),
        false
    };
}

template<typename Job, typename Configure, typename Extract>
JobResult executeJob(const QString &key,
                     CredentialJobAccess access,
                     const QString &mutationLockPath,
                     Configure configure,
                     Extract extract)
{
    QCoreApplication *application = QCoreApplication::instance();
    if (!application || QCoreApplication::closingDown()) {
        return unavailableResult(
            QStringLiteral("No running application event loop"));
    }

    const auto operation = [&]() {
        return executeJobOnCurrentThread<Job>(
            key, access, mutationLockPath,
            configure, extract);
    };
    if (QThread::currentThread() == application->thread()) {
        return operation();
    }

    JobResult result = unavailableResult(
        QStringLiteral("Cannot access application event loop"));
    QMutex resultMutex;
    const bool invoked = QMetaObject::invokeMethod(
        application,
        [&result, &resultMutex, &operation]() {
            JobResult completed = operation();
            QMutexLocker locker(&resultMutex);
            result = std::move(completed);
        },
        Qt::BlockingQueuedConnection);
    if (!invoked) {
        return unavailableResult(
            QStringLiteral("Cannot invoke credential store operation"));
    }
    QMutexLocker locker(&resultMutex);
    return result;
}

class QtKeychainCredentialStore final : public CredentialStore
{
public:
    ReadResult read(const QString &key) override
    {
        const JobResult result =
            executeJob<QKeychain::ReadPasswordJob>(
                key,
                CredentialJobAccess::ReadOnly,
                QString(),
                [](QKeychain::ReadPasswordJob *) {},
                [](QKeychain::ReadPasswordJob *job) {
                    return job->textData();
                });
        const Status status =
            CredentialStoreQtKeychainDetail::statusForError(
                result.error);
        return {status,
                status == Status::Success
                    ? result.value : QString(),
                result.errorString};
    }

    CreateResult createIfAbsent(
        const QString &key,
        const QString &value) override
    {
        Q_UNUSED(key)
        Q_UNUSED(value)
        return {
            CreateStatus::Unsupported,
            QStringLiteral(
                "Credential backend does not support atomic creation")
        };
    }

    Status write(const QString &key,
                 const QString &value,
                 QString *error) override
    {
        return writeCoordinated(
            key, value, error, QString());
    }

    Status writeCoordinated(
        const QString &key,
        const QString &value,
        QString *error,
        const QString &mutationLockPath) override
    {
        const JobResult result =
            executeJob<QKeychain::WritePasswordJob>(
                key,
                CredentialJobAccess::Mutating,
                mutationLockPath,
                [&value](QKeychain::WritePasswordJob *job) {
                    job->setTextData(value);
                },
                [](QKeychain::WritePasswordJob *) {
                    return QString();
                });
        if (error) *error = result.errorString;
        if (result.indeterminate)
            return Status::Indeterminate;
        return CredentialStoreQtKeychainDetail::statusForError(
            result.error);
    }

    Status remove(const QString &key,
                  QString *error) override
    {
        return removeCoordinated(
            key, error, QString());
    }

    Status removeCoordinated(
        const QString &key,
        QString *error,
        const QString &mutationLockPath) override
    {
        const JobResult result =
            executeJob<QKeychain::DeletePasswordJob>(
                key,
                CredentialJobAccess::Mutating,
                mutationLockPath,
                [](QKeychain::DeletePasswordJob *) {},
                [](QKeychain::DeletePasswordJob *) {
                    return QString();
                });
        if (error) *error = result.errorString;
        if (result.indeterminate)
            return Status::Indeterminate;
        return CredentialStoreQtKeychainDetail::statusForError(
            result.error);
    }
};

} // namespace

CredentialStore::Status
CredentialStoreQtKeychainDetail::statusForError(
    QKeychain::Error error)
{
    switch (error) {
    case QKeychain::NoError:
        return CredentialStore::Status::Success;
    case QKeychain::EntryNotFound:
        return CredentialStore::Status::NotFound;
    case QKeychain::AccessDeniedByUser:
    case QKeychain::AccessDenied:
    case QKeychain::NoBackendAvailable:
    case QKeychain::NotImplemented:
        return CredentialStore::Status::Unavailable;
    case QKeychain::CouldNotDeleteEntry:
    case QKeychain::OtherError:
        return CredentialStore::Status::Failed;
    }
    return CredentialStore::Status::Failed;
}

void CredentialStoreQtKeychainDetail::configureJob(
    QKeychain::Job *job,
    const QString &key)
{
    if (!job) return;
    job->setKey(key);
    job->setInsecureFallback(false);
    job->setAutoDelete(false);
}

#ifdef GC_CREDENTIAL_TEST_HOOKS
void CredentialStoreQtKeychainDetail::setJobStartHookForTest(
    JobStartHook hook)
{
    credentialJobTestHooks().startHook = std::move(hook);
}

void CredentialStoreQtKeychainDetail::setJobTimeoutHookForTest(
    JobTimeoutHook hook)
{
    credentialJobTestHooks().timeoutHook = std::move(hook);
}

void CredentialStoreQtKeychainDetail::setJobTimeoutForTest(
    int timeoutMs)
{
    credentialJobTestHooks().timeoutMs =
        timeoutMs > 0 ? timeoutMs : credentialJobTimeoutMs;
}

void CredentialStoreQtKeychainDetail::resetJobTestHooks()
{
    credentialJobTestHooks() = {};
}

void CredentialStoreQtKeychainDetail::resetJobGateForTest()
{
    CredentialJobGate &gate = credentialJobGate();
    if (!gate.mutationLockPath.isEmpty()) {
        CredentialSettingsDetail::
            removeBackendMutationMarker(
                gate.mutationLockPath);
    }
    gate.mutationLock.reset();
    gate.mutationLockPath.clear();
    gate.activeOwner = 0;
}
#endif

QByteArray CredentialStoreQtKeychainDetail::linuxRuntimeStatusReport(
    bool compileSupport,
    bool runtimeAvailable)
{
    QByteArray report(
        "goldencheetah_linux_keychain_status=1\n"
        "application=GoldenCheetah\n"
        "libsecret_compile_support=");
    report += compileSupport
        ? QByteArrayLiteral("enabled\n")
        : QByteArrayLiteral("disabled\n");
    report += QByteArrayLiteral("libsecret_runtime=");
    report += compileSupport && runtimeAvailable
        ? QByteArrayLiteral("available\n")
        : QByteArrayLiteral("unavailable\n");
    return report;
}

QString CredentialStoreQtKeychainDetail::bundledLinuxRuntimePath(
    const QString &applicationDir)
{
#ifdef Q_OS_LINUX
    const QString root =
        QFileInfo(applicationDir).canonicalFilePath();
    if (root.isEmpty()) return QString();

    const QString candidate = QDir(root).filePath(
        QStringLiteral("lib/libsecret-1.so.0"));
    const QFileInfo library(candidate);
    if (!library.isFile() || library.isSymLink()) {
        return QString();
    }
    const QString resolved = library.canonicalFilePath();
    const QString rootPrefix = root.endsWith(QDir::separator())
        ? root : root + QDir::separator();
    return resolved.startsWith(rootPrefix)
        ? resolved : QString();
#else
    Q_UNUSED(applicationDir)
    return QString();
#endif
}

void CredentialStoreQtKeychainDetail::configureBundledLinuxRuntime(
    const QString &applicationDir)
{
#ifdef Q_OS_LINUX
    const QString library =
        bundledLinuxRuntimePath(applicationDir);
    if (library.isEmpty()) {
        qunsetenv("GC_QTKEYCHAIN_LIBSECRET_PATH");
    } else {
        qputenv("GC_QTKEYCHAIN_LIBSECRET_PATH",
                QFile::encodeName(library));
    }
#else
    Q_UNUSED(applicationDir)
#endif
}

bool CredentialStoreQtKeychainDetail::linuxLibSecretCompileSupport()
{
#if defined(HAVE_LIBSECRET)
    return true;
#else
    return false;
#endif
}

bool CredentialStoreQtKeychainDetail::linuxLibSecretRuntimeAvailable()
{
#if defined(HAVE_LIBSECRET)
    return LibSecretKeyring::isAvailable();
#else
    return false;
#endif
}

#ifndef GC_CREDENTIAL_STORE_CUSTOM_FACTORY
std::unique_ptr<CredentialStore>
createPlatformCredentialStore()
{
    return createQtKeychainCredentialStore();
}
#endif

std::unique_ptr<CredentialStore>
createQtKeychainCredentialStore()
{
    return std::make_unique<QtKeychainCredentialStore>();
}
