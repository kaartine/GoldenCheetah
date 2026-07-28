#include "CredentialStoreQtKeychain.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

#if defined(HAVE_LIBSECRET)
#include <qtkeychain/libsecret_p.h>
#endif

#include <memory>

namespace {

const QString credentialService =
    QStringLiteral("org.goldencheetah.GoldenCheetah.credentials");
constexpr int credentialJobTimeoutMs = 15000;

struct JobResult
{
    QKeychain::Error error = QKeychain::OtherError;
    QString value;
    QString errorString;
};

JobResult unavailableResult(
    const QString &error)
{
    return {
        QKeychain::NoBackendAvailable,
        QString(),
        error
    };
}

template<typename Job, typename Configure, typename Extract>
JobResult executeJobOnCurrentThread(const QString &key,
                                    Configure configure,
                                    Extract extract)
{
    auto job = std::make_unique<Job>(credentialService);
    CredentialStoreQtKeychainDetail::configureJob(job.get(), key);
    configure(job.get());

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool finished = false;
    QObject::connect(job.get(), &QKeychain::Job::finished,
                     &loop, [&finished, &loop](QKeychain::Job *) {
        finished = true;
        loop.quit();
    });
    QObject::connect(&timeout, &QTimer::timeout,
                     &loop, &QEventLoop::quit);

    timeout.start(credentialJobTimeoutMs);
    job->start();
    if (!finished) loop.exec();

    if (!finished) {
        job->setAutoDelete(true);
        job.release();
        return unavailableResult(
            QStringLiteral("Credential store operation timed out"));
    }

    timeout.stop();
    return {
        job->error(),
        extract(job.get()),
        job->errorString()
    };
}

template<typename Job, typename Configure, typename Extract>
JobResult executeJob(const QString &key,
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
            key, configure, extract);
    };
    if (QThread::currentThread() == application->thread()) {
        return operation();
    }

    JobResult result = unavailableResult(
        QStringLiteral("Cannot access application event loop"));
    const bool invoked = QMetaObject::invokeMethod(
        application,
        [&result, &operation]() { result = operation(); },
        Qt::BlockingQueuedConnection);
    return invoked
        ? result
        : unavailableResult(
              QStringLiteral("Cannot invoke credential store operation"));
}

class QtKeychainCredentialStore final : public CredentialStore
{
public:
    ReadResult read(const QString &key) override
    {
        const JobResult result =
            executeJob<QKeychain::ReadPasswordJob>(
                key,
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
        const JobResult result =
            executeJob<QKeychain::WritePasswordJob>(
                key,
                [&value](QKeychain::WritePasswordJob *job) {
                    job->setTextData(value);
                },
                [](QKeychain::WritePasswordJob *) {
                    return QString();
                });
        if (error) *error = result.errorString;
        return CredentialStoreQtKeychainDetail::statusForError(
            result.error);
    }

    Status remove(const QString &key,
                  QString *error) override
    {
        const JobResult result =
            executeJob<QKeychain::DeletePasswordJob>(
                key,
                [](QKeychain::DeletePasswordJob *) {},
                [](QKeychain::DeletePasswordJob *) {
                    return QString();
                });
        if (error) *error = result.errorString;
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
