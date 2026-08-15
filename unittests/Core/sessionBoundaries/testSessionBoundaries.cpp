/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "AthleteSession.h"
#include "Context.h"
#include "SessionServices.h"
#include "TrainingSession.h"

#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFile>

#include <cstdio>
#include <memory>
#include <utility>

namespace {

class RecordingAthleteApplicationService final
    : public AthleteApplicationService
{
public:
    RecordingAthleteApplicationService(
        bool &destroyed,
        QWebEngineProfile *profile)
        : destroyed_(destroyed), profile_(profile)
    {
    }

    ~RecordingAthleteApplicationService() override
    {
        destroyed_ = true;
    }

    QWebEngineProfile *webEngineProfile() const override
    {
        return profile_;
    }

private:
    bool &destroyed_;
    QWebEngineProfile *profile_;
};

class RecordingAthletePersistenceService final
    : public AthletePersistenceService
{
public:
    explicit RecordingAthletePersistenceService(bool &destroyed)
        : destroyed_(destroyed)
    {
    }

    ~RecordingAthletePersistenceService() override
    {
        destroyed_ = true;
    }

    void reportCacheWriteFailure(
        const QString &path,
        const QString &detail) override
    {
        reportedPath = path;
        reportedDetail = detail;
    }

    QString reportedPath;
    QString reportedDetail;

private:
    bool &destroyed_;
};

class RecordingTrainingApplicationService final
    : public TrainingApplicationService
{
public:
    RecordingTrainingApplicationService(
        bool &destroyed,
        HtmlTrainingBridge *bridge)
        : destroyed_(destroyed), bridge_(bridge)
    {
    }

    ~RecordingTrainingApplicationService() override
    {
        destroyed_ = true;
    }

    HtmlTrainingBridge *htmlTrainingBridge() override
    {
        ++bridgeRequests;
        return bridge_;
    }

    int bridgeRequests = 0;

private:
    bool &destroyed_;
    HtmlTrainingBridge *bridge_;
};

struct ContextIntegrationState
{
    int applicationCreated = 0;
    int applicationDestroyed = 0;
    int profileRequests = 0;
    int profileDestroyed = 0;
    int persistenceCreated = 0;
    int persistenceDestroyed = 0;
    int trainingCreated = 0;
    int trainingDestroyed = 0;
    int bridgeRequests = 0;
    int bridgeCreated = 0;
    int bridgeDestroyed = 0;
    int persistenceReports = 0;
    QString reportedPath;
    QString reportedDetail;
};

ContextIntegrationState *activeContextIntegrationState = nullptr;

class DestructionProbe
{
public:
    explicit DestructionProbe(int &counter)
        : counter_(counter)
    {
    }

    ~DestructionProbe()
    {
        ++counter_;
    }

private:
    int &counter_;
};

class FactoryAthleteApplicationService final
    : public AthleteApplicationService
{
public:
    explicit FactoryAthleteApplicationService(
        ContextIntegrationState &state)
        : state_(state),
          profileToken_(
              std::make_unique<DestructionProbe>(
                  state.profileDestroyed))
    {
        ++state_.applicationCreated;
    }

    ~FactoryAthleteApplicationService() override
    {
        ++state_.applicationDestroyed;
    }

    QWebEngineProfile *webEngineProfile() const override
    {
        ++state_.profileRequests;
        return reinterpret_cast<QWebEngineProfile *>(profileToken_.get());
    }

private:
    ContextIntegrationState &state_;
    std::unique_ptr<DestructionProbe> profileToken_;
};

class FactoryAthletePersistenceService final
    : public AthletePersistenceService
{
public:
    FactoryAthletePersistenceService(
        ContextIntegrationState &state,
        CacheWriteFailureNotify notify)
        : state_(state), notify_(std::move(notify))
    {
        ++state_.persistenceCreated;
    }

    ~FactoryAthletePersistenceService() override
    {
        ++state_.persistenceDestroyed;
    }

    void reportCacheWriteFailure(
        const QString &path,
        const QString &detail) override
    {
        ++state_.persistenceReports;
        state_.reportedPath = path;
        state_.reportedDetail = detail;
        notify_(QStringLiteral("reported: %1").arg(path));
    }

private:
    ContextIntegrationState &state_;
    CacheWriteFailureNotify notify_;
};

class FactoryTrainingApplicationService final
    : public TrainingApplicationService
{
public:
    explicit FactoryTrainingApplicationService(
        ContextIntegrationState &state)
        : state_(state)
    {
        ++state_.trainingCreated;
    }

    ~FactoryTrainingApplicationService() override
    {
        ++state_.trainingDestroyed;
    }

    HtmlTrainingBridge *htmlTrainingBridge() override
    {
        ++state_.bridgeRequests;
        if (!bridgeToken_) {
            bridgeToken_ =
                std::make_unique<DestructionProbe>(
                    state_.bridgeDestroyed);
            ++state_.bridgeCreated;
        }
        return reinterpret_cast<HtmlTrainingBridge *>(bridgeToken_.get());
    }

private:
    ContextIntegrationState &state_;
    std::unique_ptr<DestructionProbe> bridgeToken_;
};

QStringList hostedSessionTestArguments(
    const QStringList &arguments, const QString &logPath)
{
    QStringList result = arguments;
    result << QStringLiteral("-o")
           << logPath + QStringLiteral(",txt");
    return result;
}

} // namespace

std::unique_ptr<AthleteApplicationService>
createContextAthleteApplicationService()
{
    Q_ASSERT(activeContextIntegrationState);
    return std::make_unique<FactoryAthleteApplicationService>(
        *activeContextIntegrationState);
}

std::unique_ptr<AthletePersistenceService>
createContextAthletePersistenceService(
    QObject *,
    CacheWriteFailureNotify notify)
{
    Q_ASSERT(activeContextIntegrationState);
    return std::make_unique<FactoryAthletePersistenceService>(
        *activeContextIntegrationState,
        std::move(notify));
}

std::unique_ptr<TrainingApplicationService>
createContextTrainingApplicationService(Context *)
{
    Q_ASSERT(activeContextIntegrationState);
    return std::make_unique<FactoryTrainingApplicationService>(
        *activeContextIntegrationState);
}

void connectContextMainWindow(Context *, MainWindow *)
{
}

ErgFileBase *asErgFileBase(ErgFile *workout)
{
    return reinterpret_cast<ErgFileBase *>(workout);
}

GlobalContext *GlobalContext::context()
{
    return nullptr;
}

void GlobalContext::notifyConfigChanged(qint32)
{
}

void GlobalContext::readConfig(qint32)
{
}

void GlobalContext::userMetricsConfigChanged()
{
}

class TestSessionBoundaries : public QObject
{
    Q_OBJECT

private slots:
    void hostedLoggerUsesPersistentFile();
    void contextOwnershipBoundaryIsEnforced();
    void cacheFailurePathUsesInjectedPersistencePort();
    void athleteSessionOwnsInjectedServices();
    void trainingSessionOwnsStateAndApplicationService();
    void productionContextOwnsAndDelegatesSessions();
};

void TestSessionBoundaries::hostedLoggerUsesPersistentFile()
{
    QCOMPARE(
        hostedSessionTestArguments(
            {QStringLiteral("test"), QStringLiteral("-maxwarnings"),
             QStringLiteral("0")},
            QStringLiteral("persistent.txt")),
        QStringList({QStringLiteral("test"),
                     QStringLiteral("-maxwarnings"), QStringLiteral("0"),
                     QStringLiteral("-o"),
                     QStringLiteral("persistent.txt,txt")}));
}

void TestSessionBoundaries::contextOwnershipBoundaryIsEnforced()
{
    const QString contextPath =
        QFINDTESTDATA("../../../src/Core/Context.h");
    QVERIFY2(!contextPath.isEmpty(), "Context.h test data was not found");
    QFile contextFile(contextPath);
    QVERIFY(contextFile.open(QIODevice::ReadOnly));
    const QByteArray context = contextFile.readAll();

    QVERIFY(context.contains(
        "std::unique_ptr<AthleteSession> athleteSession_"));
    QVERIFY(context.contains(
        "std::unique_ptr<TrainingSession> trainingSession_"));
    QVERIFY(!context.contains("bool isRunning;"));
    QVERIFY(!context.contains("bool isPaused;"));
    QVERIFY(!context.contains("ErgFile *workout;"));
    QVERIFY(!context.contains("VideoSyncFile *videosync;"));
    QVERIFY(!context.contains("QWebEngineProfile* webEngineProfile;"));
    QVERIFY(!context.contains("HtmlTrainingBridge *m_HtmlTrainingBridge"));
    QVERIFY(!context.contains("cacheWriteErrorCoordinator_"));
}

void TestSessionBoundaries::cacheFailurePathUsesInjectedPersistencePort()
{
    const QString cachePath =
        QFINDTESTDATA("../../../src/FileIO/RideFileCache.cpp");
    const QString rideItemPath =
        QFINDTESTDATA("../../../src/Core/RideItem.cpp");
    const QString manualActivityPath =
        QFINDTESTDATA("../../../src/Gui/ManualActivityWizard.cpp");
    QVERIFY2(!cachePath.isEmpty(), "RideFileCache.cpp test data was not found");
    QVERIFY2(!rideItemPath.isEmpty(), "RideItem.cpp test data was not found");
    QVERIFY2(
        !manualActivityPath.isEmpty(),
        "ManualActivityWizard.cpp test data was not found");

    QFile cacheFile(cachePath);
    QFile rideItemFile(rideItemPath);
    QFile manualActivityFile(manualActivityPath);
    QVERIFY(cacheFile.open(QIODevice::ReadOnly));
    QVERIFY(rideItemFile.open(QIODevice::ReadOnly));
    QVERIFY(manualActivityFile.open(QIODevice::ReadOnly));
    const QByteArray cache = cacheFile.readAll();
    const QByteArray rideItem = rideItemFile.readAll();
    const QByteArray manualActivity = manualActivityFile.readAll();

    QVERIFY(cache.contains(
        "persistenceService_->reportCacheWriteFailure"));
    QVERIFY(cache.contains(
        "context->reportCacheWriteFailure"));
    QVERIFY(cache.contains(
        "athleteSession().persistenceService()"));
    QVERIFY(rideItem.contains(
        "athleteSession().persistenceService()"));
    QVERIFY(!manualActivity.contains("&Context::workout"));
}

void TestSessionBoundaries::athleteSessionOwnsInjectedServices()
{
    bool applicationDestroyed = false;
    bool persistenceDestroyed = false;
    auto *profile = reinterpret_cast<QWebEngineProfile *>(quintptr(0x1234));
    auto persistence =
        std::make_unique<RecordingAthletePersistenceService>(
            persistenceDestroyed);
    auto *persistenceProbe = persistence.get();

    {
        AthleteSession session(
            std::make_unique<RecordingAthleteApplicationService>(
                applicationDestroyed,
                profile),
            std::move(persistence));

        QCOMPARE(session.webEngineProfile(), profile);
        QCOMPARE(&session.persistenceService(), persistenceProbe);
        session.persistenceService().reportCacheWriteFailure(
            QStringLiteral("activity.cpx"),
            QStringLiteral("disk full"));
        QCOMPARE(
            persistenceProbe->reportedPath,
            QStringLiteral("activity.cpx"));
        QCOMPARE(
            persistenceProbe->reportedDetail,
            QStringLiteral("disk full"));
        QVERIFY(!applicationDestroyed);
        QVERIFY(!persistenceDestroyed);
    }

    QVERIFY(applicationDestroyed);
    QVERIFY(persistenceDestroyed);
}

void TestSessionBoundaries::trainingSessionOwnsStateAndApplicationService()
{
    bool applicationDestroyed = false;
    auto *bridge = reinterpret_cast<HtmlTrainingBridge *>(quintptr(0x5678));
    auto application =
        std::make_unique<RecordingTrainingApplicationService>(
            applicationDestroyed,
            bridge);
    auto *applicationProbe = application.get();
    auto *workout = reinterpret_cast<ErgFile *>(quintptr(0x1111));
    auto *videoSync = reinterpret_cast<VideoSyncFile *>(quintptr(0x2222));

    {
        TrainingSession session(std::move(application));

        QVERIFY(!session.isRunning());
        QVERIFY(!session.isPaused());
        QCOMPARE(session.currentWorkout(), nullptr);
        QCOMPARE(session.currentVideoSync(), nullptr);
        QCOMPARE(session.now(), 0L);

        session.setWorkout(workout);
        session.setVideoSync(videoSync);
        session.setMediaFilename(QStringLiteral("workout.mp4"));
        session.setNow(42000L);
        session.setStatus(true, true);

        QCOMPARE(session.currentWorkout(), workout);
        QCOMPARE(session.currentVideoSync(), videoSync);
        QCOMPARE(session.mediaFilename(), QStringLiteral("workout.mp4"));
        QCOMPARE(session.now(), 42000L);
        QVERIFY(session.isRunning());
        QVERIFY(session.isPaused());
        QCOMPARE(session.htmlTrainingBridge(), bridge);
        QCOMPARE(applicationProbe->bridgeRequests, 1);
        QVERIFY(!applicationDestroyed);
    }

    QVERIFY(applicationDestroyed);
}

void TestSessionBoundaries::productionContextOwnsAndDelegatesSessions()
{
    ContextIntegrationState state;
    activeContextIntegrationState = &state;
    struct ActiveStateReset {
        ~ActiveStateReset()
        {
            activeContextIntegrationState = nullptr;
        }
    } activeStateReset;

    auto first = std::make_unique<Context>(nullptr);
    Context *const firstIdentity = first.get();
    QVERIFY(Context::isValid(firstIdentity));
    QCOMPARE(state.applicationCreated, 1);
    QCOMPARE(state.persistenceCreated, 1);
    QCOMPARE(state.trainingCreated, 1);
    QCOMPARE(state.bridgeCreated, 0);

    QWebEngineProfile *const firstProfile =
        first->webEngineProfile();
    QVERIFY(firstProfile);
    QCOMPARE(first->webEngineProfile(), firstProfile);
    QCOMPARE(state.profileRequests, 2);
    QCOMPARE(state.profileDestroyed, 0);

    HtmlTrainingBridge *const firstBridge =
        first->getHtmlTrainingBridge();
    QVERIFY(firstBridge);
    QCOMPARE(first->getHtmlTrainingBridge(), firstBridge);
    QCOMPARE(state.bridgeRequests, 2);
    QCOMPARE(state.bridgeCreated, 1);
    QCOMPARE(state.bridgeDestroyed, 0);

    auto *const firstAthlete =
        reinterpret_cast<Athlete *>(quintptr(0x1000));
    auto *const firstWorkout =
        reinterpret_cast<ErgFile *>(quintptr(0x2000));
    auto *const firstVideoSync =
        reinterpret_cast<VideoSyncFile *>(quintptr(0x3000));
    first->athlete = firstAthlete;
    ErgFile *emittedWorkout = nullptr;
    VideoSyncFile *emittedVideoSync = nullptr;
    QString emittedMedia;
    long emittedNow = 0;
    QObject::connect(
        first.get(),
        static_cast<void (Context::*)(ErgFile *)>(
            &Context::ergFileSelected),
        first.get(),
        [&](ErgFile *workout) {
            emittedWorkout = workout;
        });
    QObject::connect(
        first.get(), &Context::videoSyncFileSelected,
        first.get(),
        [&](VideoSyncFile *videoSync) {
            emittedVideoSync = videoSync;
        });
    QObject::connect(
        first.get(), &Context::mediaSelected,
        first.get(),
        [&](const QString &filename) {
            emittedMedia = filename;
        });
    QObject::connect(
        first.get(), &Context::setNow,
        first.get(),
        [&](long now) {
            emittedNow = now;
        });
    first->notifyErgFileSelected(firstWorkout);
    first->notifyVideoSyncFileSelected(firstVideoSync);
    first->notifyMediaSelected(QStringLiteral("first.mp4"));
    first->notifySetNow(42000L);
    first->setTrainingStatus(true, true);

    QCOMPARE(first->athlete, firstAthlete);
    QCOMPARE(first->currentErgFile(), firstWorkout);
    QCOMPARE(first->currentVideoSyncFile(), firstVideoSync);
    QCOMPARE(first->currentMediaFilename(), QStringLiteral("first.mp4"));
    QCOMPARE(first->getNow(), 42000L);
    QVERIFY(first->isRunning());
    QVERIFY(first->isPaused());
    QCOMPARE(emittedWorkout, firstWorkout);
    QCOMPARE(emittedVideoSync, firstVideoSync);
    QCOMPARE(emittedMedia, QStringLiteral("first.mp4"));
    QCOMPARE(emittedNow, 42000L);

    QString emittedCacheError;
    QObject::connect(
        first.get(), &Context::cacheWriteFailed,
        first.get(),
        [&](const QString &message) {
            emittedCacheError = message;
        });
    first->reportCacheWriteFailure(
        QStringLiteral("first.cpx"),
        QStringLiteral("disk full"));
    QCOMPARE(state.persistenceReports, 1);
    QCOMPARE(state.reportedPath, QStringLiteral("first.cpx"));
    QCOMPARE(state.reportedDetail, QStringLiteral("disk full"));
    QCOMPARE(emittedCacheError, QStringLiteral("reported: first.cpx"));

    auto second = std::make_unique<Context>(nullptr);
    auto *const secondAthlete =
        reinterpret_cast<Athlete *>(quintptr(0x4000));
    auto *const secondWorkout =
        reinterpret_cast<ErgFile *>(quintptr(0x5000));
    second->athlete = secondAthlete;
    second->notifyErgFileSelected(secondWorkout);
    second->notifySetNow(7L);

    QCOMPARE(second->athlete, secondAthlete);
    QCOMPARE(second->currentErgFile(), secondWorkout);
    QCOMPARE(second->getNow(), 7L);
    QCOMPARE(first->athlete, firstAthlete);
    QCOMPARE(first->currentErgFile(), firstWorkout);
    QCOMPARE(first->getNow(), 42000L);

    first.reset();
    QVERIFY(!Context::isValid(firstIdentity));
    QCOMPARE(state.applicationDestroyed, 1);
    QCOMPARE(state.profileDestroyed, 1);
    QCOMPARE(state.persistenceDestroyed, 1);
    QCOMPARE(state.trainingDestroyed, 1);
    QCOMPARE(state.bridgeDestroyed, 1);

    second.reset();
    QCOMPARE(state.applicationDestroyed, 2);
    QCOMPARE(state.profileDestroyed, 2);
    QCOMPARE(state.persistenceDestroyed, 2);
    QCOMPARE(state.trainingDestroyed, 2);
}

int main(int argc, char *argv[])
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    QCoreApplication application(argc, argv);
    TestSessionBoundaries test;
    if (!qEnvironmentVariableIsSet("CI")) {
        return QTest::qExec(&test, application.arguments());
    }

    const QString logPath = QDir(QDir::tempPath()).filePath(
        QStringLiteral("gc-session-boundaries-%1.txt").arg(
            QCoreApplication::applicationPid()));
    QFile::remove(logPath);
    const int result = QTest::qExec(
        &test,
        hostedSessionTestArguments(
            application.arguments(), logPath));

    QFile log(logPath);
    if (!log.open(QIODevice::ReadOnly)) {
        std::fprintf(
            stderr, "Could not read QtTest log: %s\n",
            qPrintable(log.errorString()));
        return result == 0 ? 2 : result;
    }
    const QByteArray output = log.readAll();
    std::fwrite(
        output.constData(), 1,
        static_cast<std::size_t>(output.size()), stderr);
    log.close();
    QFile::remove(logPath);
    return result;
}

#include "testSessionBoundaries.moc"
