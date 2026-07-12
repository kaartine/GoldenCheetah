#include "CredentialStoreQtKeychain.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

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

JobResult unavailableResult(const QString &error)
{
    return {QKeychain::NoBackendAvailable, QString(), error};
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
    return {job->error(), extract(job.get()), job->errorString()};
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
        return executeJobOnCurrentThread<Job>(key, configure, extract);
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
