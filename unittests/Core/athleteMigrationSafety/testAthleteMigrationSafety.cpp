/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "CloudAutoDownloadTestSupport.h"

#include "Cloud/CloudService.h"
#include "Cloud/NetworkReplyWait.h"
#include "Cloud/NolioTokenRefresh.h"
#include "Cloud/OAuthPKCE.h"
#include "Core/Athlete.h"
#include "Core/GcUpgrade.h"
#include "Core/Settings.h"
#include "Train/Library.h"
#include "Train/TrainDB.h"
#include "../../../contrib/qzip/zipwriter.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QPointer>
#include <QHostAddress>
#include <QSignalSpy>
#include <QSslError>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTest>
#include <QTimer>

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

#include <zlib.h>

void resetAthleteMigrationTestSettings();
void setAthleteMigrationThrowOnIdWrite(bool enabled);
void setAthleteMigrationThrowOnRideCacheConstruction(bool enabled);
Context *createAthleteMigrationTestContext();
int athleteMigrationRideFileOpenCalls();
QList<QByteArray> athleteMigrationRideFilePayloads();
void configureAthleteMigrationTrainDbUpgrade(
    TrainDB *database,
    const TrainDB::LegacyMigrationPlan &plan,
    const LibraryImportResult &result);
int athleteMigrationLibraryImportCalls();
QStringList athleteMigrationImportedFiles();
int athleteMigrationLibraryInitialiseCalls();
bool athleteMigrationLibraryInitialisedBeforeImport();
int athleteMigrationLegacyFinalizeCalls();
int athleteMigrationLegacyDropCalls();
bool athleteMigrationFinalizedExpectedImport();

namespace {

class EmptyQtThread final : public QThread
{
protected:
    void run() override {}
};

class SslWarningThread final : public QThread
{
public:
    void release() { completionGate.release(); }

protected:
    void run() override
    {
        CloudService::sslErrors(
            nullptr, nullptr,
            {QSslError(QSslError::SelfSignedCertificate)});
        completionGate.acquire();
    }

private:
    QSemaphore completionGate;
};

class OAuthRefreshThread final : public QThread
{
public:
    struct Result
    {
        bool succeeded = false;
        QString error;
        QString accessToken;
        QString refreshToken;
        int expiresIn = 0;
    };

    explicit OAuthRefreshThread(QString endpoint, int timeoutMs = -1)
        : endpoint(std::move(endpoint)),
          timeoutMs(timeoutMs)
    {
    }

    Result result() const
    {
        const std::lock_guard<std::mutex> lock(resultMutex);
        return resultValue;
    }

protected:
    void run() override
    {
        QString accessToken;
        QString refreshToken;
        int expiresIn = 0;
        QString error;
        const bool succeeded = timeoutMs >= 0
            ? OAuthPKCE::refreshAccessTokenWithTimeout(
                  endpoint, QStringLiteral("client"),
                  QStringLiteral("refresh"), accessToken, refreshToken,
                  expiresIn, error, timeoutMs)
            : OAuthPKCE::refreshAccessToken(
                  endpoint, QStringLiteral("client"),
                  QStringLiteral("refresh"), accessToken, refreshToken,
                  expiresIn, error);

        const std::lock_guard<std::mutex> lock(resultMutex);
        resultValue = {
            succeeded,
            std::move(error),
            std::move(accessToken),
            std::move(refreshToken),
            expiresIn};
    }

private:
    QString endpoint;
    int timeoutMs;
    mutable std::mutex resultMutex;
    Result resultValue;
};

class NolioRefreshThread final : public QThread
{
public:
    NolioRefreshThread(
            QString inputToken,
            std::atomic<int> &operationCalls,
            QSemaphore &operationStarted,
            QSemaphore &releaseOperation)
        : inputToken(std::move(inputToken)),
          operationCalls(operationCalls),
          operationStarted(operationStarted),
          releaseOperation(releaseOperation)
    {
    }

    NolioTokenRefreshResult result() const
    {
        const std::lock_guard<std::mutex> lock(resultMutex);
        return resultValue;
    }

protected:
    void run() override
    {
        NolioTokenRefreshResult result = NolioTokenRefreshCoordinator::refresh(
            inputToken,
            [this]() {
                const int call =
                        operationCalls.fetch_add(
                            1, std::memory_order_relaxed) + 1;
                operationStarted.release();
                releaseOperation.acquire();
                NolioTokenRefreshResult result;
                result.success = true;
                result.accessToken =
                        QStringLiteral("access-%1").arg(call);
                result.refreshToken =
                        QStringLiteral("refresh-%1").arg(call);
                result.refreshedAt = QStringLiteral("now");
                return result;
            },
            [this]() {
                return isInterruptionRequested();
            });

        const std::lock_guard<std::mutex> lock(resultMutex);
        resultValue = std::move(result);
    }

private:
    QString inputToken;
    std::atomic<int> &operationCalls;
    QSemaphore &operationStarted;
    QSemaphore &releaseOperation;
    mutable std::mutex resultMutex;
    NolioTokenRefreshResult resultValue;
};

class AbortProbeReply final : public QNetworkReply
{
public:
    explicit AbortProbeReply(QObject *parent)
        : QNetworkReply(parent)
    {
        setOpenMode(QIODevice::ReadOnly);
    }

    void abort() override
    {
        ++abortCount;
        setFinished(true);
    }

    int abortCalls() const { return abortCount; }
    void finish() { setFinished(true); }

protected:
    qint64 readData(char *, qint64) override { return -1; }

private:
    int abortCount = 0;
};

class AbortProbeService final : public CloudService
{
public:
    AbortProbeService() : CloudService(nullptr) {}

    CloudService *clone(Context *) override
    {
        return new AbortProbeService();
    }

    QString id() const override
    {
        return QStringLiteral("AbortProbe");
    }

    QImage logo() const override { return {}; }
};

QByteArray gzipPayload(const QByteArray &source)
{
    z_stream stream = {};
    if (deflateInit2(
            &stream, Z_BEST_COMPRESSION, Z_DEFLATED,
            15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return {};
    }

    QByteArray output(
        int(compressBound(uLong(source.size())) + 32), '\0');
    stream.avail_in = uInt(source.size());
    stream.next_in = reinterpret_cast<Bytef *>(
        const_cast<char *>(source.constData()));
    stream.avail_out = uInt(output.size());
    stream.next_out = reinterpret_cast<Bytef *>(output.data());

    const int result = deflate(&stream, Z_FINISH);
    const int outputSize = int(stream.total_out);
    deflateEnd(&stream);
    if (result != Z_STREAM_END) return {};

    output.resize(outputSize);
    return output;
}

QByteArray zipPayload(const QByteArray &source)
{
    QTemporaryFile archive;
    if (!archive.open()) return {};

    {
        ZipWriter writer(&archive);
        writer.addFile(QStringLiteral("activity.fit"), source);
        writer.close();
    }
    QFile completedArchive(archive.fileName());
    if (!completedArchive.open(QIODevice::ReadOnly)) return {};
    return completedArchive.readAll();
}

class SeededAthleteStorage
{
public:
    SeededAthleteStorage()
    {
        storage.fill(std::byte{0xa5});
    }

    ~SeededAthleteStorage()
    {
        if (athlete) athlete->~Athlete();
    }

    Athlete *construct(Context *context, const QDir &home)
    {
        Q_ASSERT(!athlete);
        athlete = ::new (storage.data()) Athlete(context, home);
        return athlete;
    }

    void destroy()
    {
        Q_ASSERT(athlete);
        athlete->~Athlete();
        athlete = nullptr;
        destructionCompleted = true;
    }

    bool constructed() const { return athlete != nullptr; }
    bool destroyed() const { return destructionCompleted; }

private:
    alignas(Athlete) std::array<std::byte, sizeof(Athlete)> storage;
    Athlete *athlete = nullptr;
    bool destructionCompleted = false;
};

template<typename Predicate>
bool waitUntil(Predicate predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QTest::qWait(5);
    }
    return predicate();
}

enum class ModalResponseType {
    Dialog,
    MessageBox
};

struct ModalResponse
{
    ModalResponseType type;
    int value;

    static ModalResponse dialog(int result)
    {
        return { ModalResponseType::Dialog, result };
    }

    static ModalResponse message(QMessageBox::StandardButton button)
    {
        return { ModalResponseType::MessageBox, int(button) };
    }
};

struct ModalSequenceState
{
    QList<ModalResponse> responses;
    int *handled = nullptr;
};

void pollModalSequence(const std::shared_ptr<ModalSequenceState> &state)
{
    QWidget *modal = QApplication::activeModalWidget();
    if (!modal) {
        QTimer::singleShot(0, [state]() { pollModalSequence(state); });
        return;
    }

    const ModalResponse response = state->responses.takeFirst();
    bool answered = false;

    if (response.type == ModalResponseType::Dialog) {
        if (auto *dialog = qobject_cast<QDialog *>(modal)) {
            dialog->done(response.value);
            answered = true;
        }
    } else if (auto *messageBox = qobject_cast<QMessageBox *>(modal)) {
        QAbstractButton *button = messageBox->button(
            QMessageBox::StandardButton(response.value));
        if (button) {
            button->click();
            answered = true;
        }
    }

    if (answered) ++*state->handled;
    if (!state->responses.isEmpty()) {
        QTimer::singleShot(0, [state]() { pollModalSequence(state); });
    }
}

void answerModalSequence(
    std::initializer_list<ModalResponse> responses, int &handled)
{
    handled = 0;
    auto state = std::make_shared<ModalSequenceState>();
    state->responses = QList<ModalResponse>(
        responses.begin(), responses.end());
    state->handled = &handled;
    QTimer::singleShot(0, [state]() { pollModalSequence(state); });
}

void answerNextMessageBox(
    QMessageBox::StandardButton answer, bool &handled)
{
    handled = false;
    QTimer::singleShot(0, [answer, &handled]() {
        auto *messageBox = qobject_cast<QMessageBox *>(
            QApplication::activeModalWidget());
        if (messageBox) {
            QAbstractButton *button = messageBox->button(answer);
            if (button) {
                handled = true;
                button->click();
            }
        }
    });
}


struct GuiMessageResponseState
{
    bool active = true;
    bool *handled = nullptr;
    bool *shownOnGuiThread = nullptr;
};

void pollGuiMessageResponse(
        const std::shared_ptr<GuiMessageResponseState> &state)
{
    if (!state->active) return;

    auto *messageBox = qobject_cast<QMessageBox *>(
        QApplication::activeModalWidget());
    if (!messageBox) {
        QTimer::singleShot(
            0, [state]() { pollGuiMessageResponse(state); });
        return;
    }

    *state->shownOnGuiThread =
            messageBox->thread() == QApplication::instance()->thread();
    if (QAbstractButton *button = messageBox->button(QMessageBox::Ok)) {
        *state->handled = true;
        state->active = false;
        button->click();
    }
}

std::shared_ptr<GuiMessageResponseState>
answerNextMessageBoxOnGuiThread(
        bool &handled, bool &shownOnGuiThread)
{
    handled = false;
    shownOnGuiThread = false;
    auto state = std::make_shared<GuiMessageResponseState>();
    state->handled = &handled;
    state->shownOnGuiThread = &shownOnGuiThread;
    QTimer::singleShot(
        0, [state]() { pollGuiMessageResponse(state); });
    return state;
}

bool createStructuredAthlete(QDir &athleteDir, const QString &name)
{
    if (!athleteDir.mkdir(name) || !athleteDir.cd(name)
        || !athleteDir.mkdir(QStringLiteral("activities"))
        || !athleteDir.mkdir(QStringLiteral("config"))) {
        return false;
    }

    QFile configMarker(
        athleteDir.filePath(QStringLiteral("config/athlete.xml")));
    if (!configMarker.open(QIODevice::WriteOnly)) return false;
    return configMarker.write("test") == 4;
}

void configureAthlete(
    const QDir &athleteDir, int version, bool folderUpgradeComplete)
{
    const QString athlete = athleteDir.dirName();
    appsettings->setCValue(
        athlete, GC_UPGRADE_FOLDER_SUCCESS, folderUpgradeComplete);
    appsettings->setCValue(athlete, GC_VERSION_USED, version);
}

} // namespace

class TestAthleteMigrationSafety : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void folderUpgradeRejectionSkipsAction();
    void folderAcceptanceThenCompatibilityRejectionSkipsAction();
    void allRequiredPromptsAcceptedRunActionOnce();
    void cancelledCompatibilityStopsBeforeAthleteConstruction();
    void acceptedCompatibilityAllowsAthleteConstruction();
    void currentVersionNeedsNoCompatibilityPrompt();
    void newAthleteNeedsNoCompatibilityPrompt();
    void lateUpgradeWithoutTrainDbSucceeds();
    void lateUpgradeFinalizesVerifiedTrainDbMigration();
    void publishedContextRollsBackLateConstructionFailure();
    void constructorFailureRollsBackPublishedContextAndOwners();
    void invalidContextStopsCloudDownloadPromptly();
    void athleteTeardownJoinsBlockedCloudDownload();
    void synchronousCloudCompletionIsPromptAndThreadIsolated();
    void inlineCloudCompletionDoesNotReenterProvider();
    void queuedCloudResultsSurviveWorkerCleanup();
    void naturalCloudCompletionStopsWithoutGuiDispatch();
    void queuedCloudSettingsPreserveNewerValues();
    void queuedCloudSettingsRejectRemovedExpectedValue();
    void sequentialQueuedCloudSettingsPreserveLatestValues();
    void sequentialQueuedCloudSettingsRejectConflictChain();
    void guiCloudSettingsSaveWritesAllScopes();
    void guiCloudSettingsSaveClearsCustomUrl();
    void workerCloudSettingsClearUrlUsesDefault();
    void defaultCloudUrlDoesNotInvalidatePayload();
    void emptyCloudUrlUsesDefault();
    void changedCloudUrlInvalidatesPayload();
    void queuedCloudPayloadIsDiscardedWhenStartupSyncIsDisabled();
    void progressCallbackCanDestroyDownloader();
    void timedOutCloudReadReleasesBuffer();
    void rejectedCloudReadReleasesBufferPromptly();
    void duplicateCloudCompletionIsAppliedOnceAcrossRepeatedRuns();
    void providerCancellationDoesNotDestroyActiveCall_data();
    void providerCancellationDoesNotDestroyActiveCall();
    void cancellationFromStartSignalStopsSafely();
    void staleQueuedResultsAreDiscardedAfterRestart();
    void legacyReadCompleteDoesNotTakeCallerOwnership();
    void noEnabledCloudServiceEmitsNoLifecycleSignals();
    void autoDownloadEndFollowsImportsAndProgress();
    void plainQtThreadLifecycleIsJoined();
    void baseCloudAbortStopsRunningReplies();
    void cloudCompressionModesAreExtracted();
    void nolioTokenRefreshIsSingleFlight();
    void nolioTokenRefreshCacheExpires();
    void nolioTokenRefreshFollowerCanCancel();
    void networkReplyWaitTimesOut();
    void networkReplyWaitPrefersPreexistingInterruption();
    void oauthRefreshHandlesImmediateReply();
    void oauthRefreshTimesOut();
    void oauthRefreshHonorsThreadInterruption();
    void sslWarningsAreMarshaledToGuiThread();
};

void TestAthleteMigrationSafety::init()
{
    resetAthleteMigrationTestSettings();
}

void TestAthleteMigrationSafety::folderUpgradeRejectionSkipsAction()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("FolderReject")));
    configureAthlete(athleteDir, VERSION35_BUILD, false);

    int handled = 0;
    int actionCount = 0;
    answerModalSequence(
        { ModalResponse::dialog(QDialog::Rejected) }, handled);

    GcUpgrade upgrade;
    QVERIFY(!upgrade.executeAfterConfirmation(
        athleteDir, [&actionCount]() { ++actionCount; }));
    QCOMPARE(handled, 1);
    QCOMPARE(actionCount, 0);
}

void TestAthleteMigrationSafety::
folderAcceptanceThenCompatibilityRejectionSkipsAction()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("CompatibilityReject")));
    configureAthlete(athleteDir, VERSION35_BUILD, false);

    int handled = 0;
    int actionCount = 0;
    answerModalSequence(
        {
            ModalResponse::dialog(QDialog::Accepted),
            ModalResponse::message(QMessageBox::Cancel)
        },
        handled);

    GcUpgrade upgrade;
    QVERIFY(!upgrade.executeAfterConfirmation(
        athleteDir, [&actionCount]() { ++actionCount; }));
    QCOMPARE(handled, 2);
    QCOMPARE(actionCount, 0);
}

void TestAthleteMigrationSafety::allRequiredPromptsAcceptedRunActionOnce()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("AllAccepted")));
    configureAthlete(athleteDir, VERSION35_BUILD, false);

    int handled = 0;
    int actionCount = 0;
    answerModalSequence(
        {
            ModalResponse::dialog(QDialog::Accepted),
            ModalResponse::message(QMessageBox::Ok)
        },
        handled);

    GcUpgrade upgrade;
    QVERIFY(upgrade.executeAfterConfirmation(
        athleteDir, [&actionCount]() { ++actionCount; }));
    QCOMPARE(handled, 2);
    QCOMPARE(actionCount, 1);
}

void TestAthleteMigrationSafety::
cancelledCompatibilityStopsBeforeAthleteConstruction()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("LegacyAthleteCancel")));
    configureAthlete(athleteDir, VERSION35_BUILD, true);

    bool promptHandled = false;
    answerNextMessageBox(QMessageBox::Cancel, promptHandled);
    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;

    GcUpgrade upgrade;
    QVERIFY(!upgrade.executeAfterConfirmation(
        athleteDir,
        [&]() { athleteStorage.construct(context.get(), athleteDir); }));

    QVERIFY(promptHandled);
    QVERIFY(!athleteStorage.constructed());
    QVERIFY(!athleteStorage.destroyed());
    QVERIFY(!context->athlete);
}

void TestAthleteMigrationSafety::
acceptedCompatibilityAllowsAthleteConstruction()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("LegacyAthleteAccept")));
    configureAthlete(athleteDir, VERSION35_BUILD, true);

    bool promptHandled = false;
    answerNextMessageBox(QMessageBox::Ok, promptHandled);
    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;

    Athlete *athleteInstance = nullptr;
    GcUpgrade upgrade;
    QVERIFY(upgrade.executeAfterConfirmation(
        athleteDir,
        [&]() {
            athleteInstance =
                athleteStorage.construct(context.get(), athleteDir);
        }));
    QVERIFY(promptHandled);
    QCOMPARE(context->athlete, athleteInstance);

    athleteStorage.destroy();
    QVERIFY(athleteStorage.destroyed());
    QVERIFY(!context->athlete);
}

void TestAthleteMigrationSafety::currentVersionNeedsNoCompatibilityPrompt()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("CurrentAthlete")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    bool unexpectedPrompt = false;
    answerNextMessageBox(QMessageBox::Cancel, unexpectedPrompt);
    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;

    GcUpgrade upgrade;
    QVERIFY(upgrade.executeAfterConfirmation(
        athleteDir,
        [&]() { athleteStorage.construct(context.get(), athleteDir); }));
    QCoreApplication::processEvents();
    QVERIFY(!unexpectedPrompt);
    QVERIFY(athleteStorage.constructed());

    athleteStorage.destroy();
    QVERIFY(!context->athlete);
}

void TestAthleteMigrationSafety::newAthleteNeedsNoCompatibilityPrompt()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("NewAthlete")));
    configureAthlete(athleteDir, 0, true);

    bool unexpectedPrompt = false;
    answerNextMessageBox(QMessageBox::Cancel, unexpectedPrompt);
    std::unique_ptr<Context> context;
    Context *publishedContext = nullptr;
    int actionCount = 0;
    int publishCount = 0;
    int rollbackCount = 0;

    GcUpgrade upgrade;
    QVERIFY(upgrade.executeAfterConfirmation(
        athleteDir,
        [&]() {
            ++actionCount;
            context.reset(Athlete::createInNewContext(
                nullptr, athleteDir,
                [&](Context *candidate) {
                    ++publishCount;
                    publishedContext = candidate;
                },
                [&](Context *) { ++rollbackCount; }));
        }));
    QCoreApplication::processEvents();

    QVERIFY(!unexpectedPrompt);
    QCOMPARE(actionCount, 1);
    QCOMPARE(publishCount, 1);
    QCOMPARE(rollbackCount, 0);
    QCOMPARE(publishedContext, context.get());
    QVERIFY(context->athlete);

    delete context->athlete;
    QVERIFY(!context->athlete);
}

void TestAthleteMigrationSafety::lateUpgradeWithoutTrainDbSucceeds()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("LateUpgradeNoTrainDb")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete =
        athleteStorage.construct(context.get(), athleteDir);
    QCOMPARE(context->athlete, athlete);

    GcUpgrade upgrade;
    QCOMPARE(upgrade.upgradeLate(context.get()), 0);

    athleteStorage.destroy();
    QVERIFY(!context->athlete);
}

void TestAthleteMigrationSafety::lateUpgradeFinalizesVerifiedTrainDbMigration()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    gcroot = root.path();
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("LateUpgradeTrainDb")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    athleteStorage.construct(context.get(), athleteDir);

    TrainDB database(QDir(root.path()));
    TrainDB::LegacyMigrationPlan plan;
    plan.valid = true;
    plan.workouts = {QStringLiteral("/legacy.erg")};
    plan.videos = {QStringLiteral("/legacy.mp4")};
    plan.videoSyncs = {QStringLiteral("/legacy.rlv")};

    LibraryImportResult result;
    result.completed = true;
    result.requestedFiles = plan.files();
    result.importedWorkouts.insert(
        plan.workouts.first(), QStringLiteral("/library/legacy.erg"));
    result.importedVideos.insert(plan.videos.first(), plan.videos.first());
    result.importedVideoSyncs.insert(
        plan.videoSyncs.first(), QStringLiteral("/library/legacy.rlv"));
    configureAthleteMigrationTrainDbUpgrade(&database, plan, result);

    GcUpgrade upgrade;
    QCOMPARE(upgrade.upgradeLate(context.get()), 0);
    QCOMPARE(athleteMigrationLibraryInitialiseCalls(), 1);
    QVERIFY(athleteMigrationLibraryInitialisedBeforeImport());
    QCOMPARE(athleteMigrationLibraryImportCalls(), 1);
    QCOMPARE(athleteMigrationImportedFiles(), plan.files());
    QCOMPARE(athleteMigrationLegacyFinalizeCalls(), 1);
    QCOMPARE(athleteMigrationLegacyDropCalls(), 0);
    QVERIFY(athleteMigrationFinalizedExpectedImport());

    trainDB = nullptr;
    athleteStorage.destroy();
    QVERIFY(!context->athlete);
}

void TestAthleteMigrationSafety::
publishedContextRollsBackLateConstructionFailure()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("LateConstructionFailure")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    setAthleteMigrationThrowOnRideCacheConstruction(true);
    Context *publishedContext = nullptr;
    Context *unexpectedContext = nullptr;
    int publishCount = 0;
    int rollbackCount = 0;
    bool rollbackContextMatched = false;
    bool rollbackSawClearedAthlete = false;
    bool exceptionCaught = false;

    try {
        unexpectedContext = Athlete::createInNewContext(
            nullptr, athleteDir,
            [&](Context *candidate) {
                ++publishCount;
                publishedContext = candidate;
            },
            [&](Context *candidate) {
                ++rollbackCount;
                rollbackContextMatched = candidate == publishedContext;
                rollbackSawClearedAthlete = !candidate->athlete;
                publishedContext = nullptr;
            });
    } catch (const std::runtime_error &) {
        exceptionCaught = true;
    }
    setAthleteMigrationThrowOnRideCacheConstruction(false);

    if (unexpectedContext) {
        delete unexpectedContext->athlete;
        delete unexpectedContext;
    }

    QVERIFY(exceptionCaught);
    QCOMPARE(publishCount, 1);
    QCOMPARE(rollbackCount, 1);
    QVERIFY(rollbackContextMatched);
    QVERIFY(rollbackSawClearedAthlete);
    QVERIFY(!publishedContext);
}

void TestAthleteMigrationSafety::
constructorFailureRollsBackPublishedContextAndOwners()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("ThrowingAthlete")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    setAthleteMigrationThrowOnIdWrite(true);

    bool exceptionCaught = false;
    try {
        athleteStorage.construct(context.get(), athleteDir);
    } catch (const std::runtime_error &) {
        exceptionCaught = true;
    }
    setAthleteMigrationThrowOnIdWrite(false);

    QVERIFY(exceptionCaught);
    QVERIFY(!athleteStorage.constructed());
    QVERIFY(!athleteStorage.destroyed());
    QVERIFY(!context->athlete);
}

void TestAthleteMigrationSafety::invalidContextStopsCloudDownloadPromptly()
{
    Context *context = createAthleteMigrationTestContext();
    CloudServiceAutoDownload worker(context);
    delete context;

    worker.checkDownload();
    const bool stoppedPromptly = worker.wait(500);
    bool cleanupStopped = stoppedPromptly;
    if (!cleanupStopped) cleanupStopped = worker.wait(5000);

    QVERIFY2(cleanupStopped,
             "cloud worker did not finish during test cleanup");
    QVERIFY2(stoppedPromptly,
             "cloud worker continued after its context became invalid");
}

void TestAthleteMigrationSafety::athleteTeardownJoinsBlockedCloudDownload()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudDownloadLifecycle")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureBlockingCloudAutoDownload(athlete->cyclist);
    QPointer<CloudServiceAutoDownload> worker(athlete->cloudAutoDownload);
    worker->checkDownload();

    const bool readStarted = waitForBlockingCloudRead(5000);
    if (!readStarted) {
        athleteStorage.destroy();
        if (worker) {
            worker->wait(35000);
            delete worker.data();
        }
        cleanupBlockingCloudRead();
    }
    QVERIFY2(readStarted, "production cloud worker did not reach readFile");

    QElapsedTimer teardownTimer;
    teardownTimer.start();
    athleteStorage.destroy();
    const qint64 teardownElapsedMs = teardownTimer.elapsed();
    const bool joinedDuringTeardown = worker.isNull();

    bool releaseSucceeded = true;
    bool cleanupJoined = true;
    if (worker) {
        releaseSucceeded = releaseBlockingCloudRead(worker.data());
        cleanupJoined = worker->wait(7000);
        if (!cleanupJoined) cleanupJoined = worker->wait(35000);
        if (cleanupJoined) delete worker.data();
    }
    const int buffersAllocated = cloudAutoDownloadTestBuffersAllocated();
    const int buffersReleased = cloudAutoDownloadTestBuffersReleased();
    const int buffersOutstanding = cloudAutoDownloadTestBuffersOutstanding();
    cleanupBlockingCloudRead();

    QVERIFY2(releaseSucceeded, "failed to release blocked test provider");
    QVERIFY2(cleanupJoined, "cloud worker did not finish during test cleanup");
    QVERIFY2(joinedDuringTeardown,
             "Athlete teardown returned while cloud worker was still running");
    QVERIFY2(teardownElapsedMs < 2000,
             "Athlete teardown did not cancel and join within two seconds");
    QCOMPARE(buffersAllocated, 1);
    QCOMPARE(buffersReleased, 1);
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::
synchronousCloudCompletionIsPromptAndThreadIsolated()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudInlineCompletion")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Inline, 1, 50);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    const bool spyValid = finished.isValid();
    athlete->cloudAutoDownload->checkDownload();

    const bool readStarted = waitForControlledCloudReads(1, 5000);
    const bool completionEmitted =
            waitForControlledCloudCompletions(1, 5000);
    const bool resultParsed = waitUntil(
            []() { return athleteMigrationRideFileOpenCalls() == 1; },
            1000);
    const bool finishedPromptly = waitUntil(
            [&finished]() { return finished.count() == 1; }, 1000);
    const bool athleteSettingSaved = waitUntil(
            [athlete]() {
                return appsettings->cvalue(
                    athlete->cyclist,
                    QStringLiteral("<athlete-private>controlled/thread"))
                           .toString() == QStringLiteral("worker-value");
            },
            1000);
    const bool globalSettingSaved = waitUntil(
            []() {
                return appsettings->value(
                    nullptr,
                    QStringLiteral("<global-general>controlled/thread"))
                           .toString() ==
                        QStringLiteral("global-worker-value");
            },
            1000);
    const int crossThreadProviderAccesses =
            controlledCloudCrossThreadProviderAccesses();
    const int destroyedProviderAccesses =
            controlledCloudDestroyedProviderAccesses();
    const int providerOperations =
            controlledCloudProviderOperations();
    const int crossThreadSettingsWrites =
            athleteMigrationSettingsCrossThreadWrites();
    const int buffersAllocated = cloudAutoDownloadTestBuffersAllocated();
    const int buffersReleased = cloudAutoDownloadTestBuffersReleased();
    const int buffersOutstanding = cloudAutoDownloadTestBuffersOutstanding();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(spyValid);
    QVERIFY2(readStarted, "production cloud worker did not reach readFile");
    QVERIFY2(completionEmitted,
             "inline cloud completion was not emitted");
    QVERIFY2(resultParsed,
             "inline cloud result was not delivered for parsing");
    QVERIFY2(finishedPromptly,
             "inline cloud completion was lost before the wait began");
    QVERIFY2(athleteSettingSaved,
             "worker athlete setting was not persisted on the GUI thread");
    QVERIFY2(globalSettingSaved,
             "worker global setting was not persisted on the GUI thread");
    QCOMPARE(crossThreadProviderAccesses, 0);
    QCOMPARE(destroyedProviderAccesses, 0);
    QVERIFY(providerOperations >= 7);
    QCOMPARE(crossThreadSettingsWrites, 0);
    QCOMPARE(buffersAllocated, 1);
    QCOMPARE(buffersReleased, 1);
    QCOMPARE(buffersOutstanding, 0);
}

void
TestAthleteMigrationSafety::inlineCloudCompletionDoesNotReenterProvider()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudInlineNestedEvents")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
        athlete->cyclist,
        TestCloudCompletionMode::InlineWithNestedEvents,
        2,
        500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    athlete->cloudAutoDownload->checkDownload();

    const bool readsStarted = waitForControlledCloudReads(2, 5000);
    const bool completionsEmitted =
            waitForControlledCloudCompletions(2, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool finishedDelivered = waitUntil(
        [&finished]() { return finished.count() == 1; }, 2000);
    const int reentrantReadCalls =
            controlledCloudReentrantReadCalls();
    const int buffersOutstanding =
            cloudAutoDownloadTestBuffersOutstanding();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(readsStarted);
    QVERIFY(completionsEmitted);
    QVERIFY(providerDestroyed);
    QVERIFY(finishedDelivered);
    QCOMPARE(reentrantReadCalls, 0);
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::queuedCloudResultsSurviveWorkerCleanup()
{
    constexpr int EntryCount = 8;

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudQueuedCompletion")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist,
            TestCloudCompletionMode::Queued,
            EntryCount,
            500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    const bool spyValid = finished.isValid();
    athlete->cloudAutoDownload->checkDownload();

    const bool allReadsStarted =
            waitForControlledCloudReads(EntryCount, 5000);
    const bool allCompletionsEmitted =
            waitForControlledCloudCompletions(EntryCount, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(6000);
    const bool finishedDelivered = waitUntil(
            [&finished]() { return finished.count() == 1; }, 1000);
    const bool allResultsParsed = waitUntil(
            []() {
                return athleteMigrationRideFileOpenCalls() == EntryCount;
            },
            1000);
    const int crossThreadProviderAccesses =
            controlledCloudCrossThreadProviderAccesses();
    const int destroyedProviderAccesses =
            controlledCloudDestroyedProviderAccesses();
    const int buffersAllocated = cloudAutoDownloadTestBuffersAllocated();
    const int buffersReleased = cloudAutoDownloadTestBuffersReleased();
    const int buffersOutstanding = cloudAutoDownloadTestBuffersOutstanding();
    const QList<QByteArray> parsedPayloads =
            athleteMigrationRideFilePayloads();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(spyValid);
    QVERIFY2(allReadsStarted,
             "not all queued cloud reads were started");
    QVERIFY2(allCompletionsEmitted,
             "not all queued cloud completions were emitted");
    QVERIFY2(providerDestroyed,
             "cloud provider was not cleaned up by the worker");
    QVERIFY2(finishedDelivered,
             "queued cloud download did not report completion");
    QVERIFY2(allResultsParsed,
             "queued cloud results were lost during worker cleanup");
    QCOMPARE(crossThreadProviderAccesses, 0);
    QCOMPARE(destroyedProviderAccesses, 0);
    QCOMPARE(parsedPayloads.size(), EntryCount);
    for (const QByteArray &payload : parsedPayloads) {
        QCOMPARE(payload, QByteArray("not-a-valid-fit"));
    }
    QCOMPARE(buffersAllocated, EntryCount);
    QCOMPARE(buffersReleased, EntryCount);
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::
naturalCloudCompletionStopsWithoutGuiDispatch()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudNaturalThreadExit")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Queued, 1, 500);
    CloudServiceAutoDownload *worker = athlete->cloudAutoDownload;
    worker->checkDownload();

    const bool readStarted = waitForControlledCloudReads(1, 5000);
    const bool completionEmitted =
            waitForControlledCloudCompletions(1, 5000);
    const bool providerDestroyedBeforeGuiDispatch =
            waitForControlledCloudProviderDestroyed(5000);
    const bool stoppedNaturally = worker->wait(1000);
    worker->cancelAndWait();
    const bool providerDestroyedDuringCleanup =
            providerDestroyedBeforeGuiDispatch
            || waitForControlledCloudProviderDestroyed(1000);
    const int buffersOutstanding =
            cloudAutoDownloadTestBuffersOutstanding();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(readStarted);
    QVERIFY(completionEmitted);
    QVERIFY2(providerDestroyedDuringCleanup,
             "cloud provider was not destroyed during cleanup");
    QVERIFY2(providerDestroyedBeforeGuiDispatch,
             "worker cleanup depended on the GUI completion callback");
    QVERIFY2(stoppedNaturally,
             "QThread::wait blocked before GUI completion dispatch");
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::
queuedCloudSettingsPreserveNewerValues()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudSettingsConflict")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    appsettings->setValue(
        QStringLiteral("<global-general>controlled/thread"),
        QStringLiteral("initial-global-value"));
    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Queued, 1, 500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    athlete->cloudAutoDownload->checkDownload();

    const bool completionEmitted =
            waitForControlledCloudCompletions(1, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    appsettings->setCValue(
        athlete->cyclist,
        QStringLiteral("<athlete-private>controlled/thread"),
        QStringLiteral("newer-athlete-value"));

    const bool finishedDelivered = waitUntil(
            [&finished]() { return finished.count() == 1; }, 2000);
    const QString athleteValue = appsettings->cvalue(
        athlete->cyclist,
        QStringLiteral("<athlete-private>controlled/thread")).toString();
    const QString globalValue = appsettings->value(
        nullptr,
        QStringLiteral("<global-general>controlled/thread")).toString();
    const int crossThreadWrites =
            athleteMigrationSettingsCrossThreadWrites();
    const int parsedResults =
            athleteMigrationRideFileOpenCalls();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(completionEmitted);
    QVERIFY(providerDestroyed);
    QVERIFY(finishedDelivered);
    QCOMPARE(athleteValue, QStringLiteral("newer-athlete-value"));
    QCOMPARE(globalValue, QStringLiteral("initial-global-value"));
    QCOMPARE(crossThreadWrites, 0);
    QCOMPARE(parsedResults, 0);
}

void TestAthleteMigrationSafety::
queuedCloudSettingsRejectRemovedExpectedValue()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudSettingsRemoval")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    const QString globalKey =
            QStringLiteral("<global-general>controlled/thread");
    appsettings->setValue(
        globalKey, QStringLiteral("initial-global-value"));
    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Queued, 1, 500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    athlete->cloudAutoDownload->checkDownload();

    const bool completionEmitted =
            waitForControlledCloudCompletions(1, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    appsettings->remove(globalKey);

    const bool finishedDelivered = waitUntil(
            [&finished]() { return finished.count() == 1; }, 2000);
    const QString globalValue = appsettings->value(
        nullptr, globalKey, QStringLiteral("missing")).toString();
    const QString athleteValue = appsettings->cvalue(
        athlete->cyclist,
        QStringLiteral("<athlete-private>controlled/thread"),
        QStringLiteral("missing")).toString();
    const int parsedResults = athleteMigrationRideFileOpenCalls();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(completionEmitted);
    QVERIFY(providerDestroyed);
    QVERIFY(finishedDelivered);
    QCOMPARE(globalValue, QStringLiteral("missing"));
    QCOMPARE(athleteValue, QStringLiteral("missing"));
    QCOMPARE(parsedResults, 0);
}

void TestAthleteMigrationSafety::
sequentialQueuedCloudSettingsPreserveLatestValues()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudSettingsSequence")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    appsettings->setCValue(
        athlete->cyclist,
        QStringLiteral("<athlete-private>controlled/thread"),
        QStringLiteral("initial-athlete-value"));
    appsettings->setValue(
        QStringLiteral("<global-general>controlled/thread"),
        QStringLiteral("initial-global-value"));
    configureControlledCloudAutoDownload(
            athlete->cyclist,
            TestCloudCompletionMode::QueuedSettingsTwice, 1, 500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    athlete->cloudAutoDownload->checkDownload();

    const bool completionEmitted =
            waitForControlledCloudCompletions(1, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool finishedDelivered = waitUntil(
            [&finished]() { return finished.count() == 1; }, 2000);
    const QString athleteValue = appsettings->cvalue(
        athlete->cyclist,
        QStringLiteral("<athlete-private>controlled/thread")).toString();
    const QString globalValue = appsettings->value(
        nullptr,
        QStringLiteral("<global-general>controlled/thread")).toString();
    const int parsedResults = athleteMigrationRideFileOpenCalls();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(completionEmitted);
    QVERIFY(providerDestroyed);
    QVERIFY(finishedDelivered);
    QCOMPARE(athleteValue, QStringLiteral("worker-value-2"));
    QCOMPARE(globalValue, QStringLiteral("global-worker-value-2"));
    QCOMPARE(parsedResults, 1);
}

void TestAthleteMigrationSafety::
sequentialQueuedCloudSettingsRejectConflictChain()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudSettingsConflictChain")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    const QString athleteKey =
            QStringLiteral("<athlete-private>controlled/thread");
    const QString globalKey =
            QStringLiteral("<global-general>controlled/thread");
    appsettings->setCValue(
        athlete->cyclist, athleteKey,
        QStringLiteral("initial-athlete-value"));
    appsettings->setValue(
        globalKey, QStringLiteral("initial-global-value"));
    configureControlledCloudAutoDownload(
            athlete->cyclist,
            TestCloudCompletionMode::QueuedSettingsTwice, 1, 500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    athlete->cloudAutoDownload->checkDownload();

    const bool completionEmitted =
            waitForControlledCloudCompletions(1, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    appsettings->setCValue(
        athlete->cyclist, athleteKey,
        QStringLiteral("external-athlete-value"));
    const bool finishedDelivered = waitUntil(
            [&finished]() { return finished.count() == 1; }, 2000);
    const QString athleteValue = appsettings->cvalue(
        athlete->cyclist, athleteKey).toString();
    const QString globalValue = appsettings->value(
        nullptr, globalKey).toString();
    const int parsedResults = athleteMigrationRideFileOpenCalls();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(completionEmitted);
    QVERIFY(providerDestroyed);
    QVERIFY(finishedDelivered);
    QCOMPARE(
        athleteValue, QStringLiteral("external-athlete-value"));
    QCOMPARE(globalValue, QStringLiteral("initial-global-value"));
    QCOMPARE(parsedResults, 0);
}

void TestAthleteMigrationSafety::guiCloudSettingsSaveWritesAllScopes()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudGuiSettings")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
        athlete->cyclist, TestCloudCompletionMode::Queued, 1, 500);
    std::unique_ptr<CloudService> service(
        CloudServiceFactory::instance().newService(
            QStringLiteral("ControlledLifecycleCloud"), context.get()));
    QVERIFY(service);

    const QString athleteKey =
            QStringLiteral("<athlete-private>controlled/thread");
    const QString globalKey =
            QStringLiteral("<global-general>controlled/thread");
    service->setSetting(athleteKey, QStringLiteral("gui-athlete-value"));
    service->setSetting(globalKey, QStringLiteral("gui-global-value"));
    service->setSetting(
        service->syncOnStartupSettingName(), QStringLiteral("false"));
    service->setSetting(
        service->syncOnImportSettingName(), QStringLiteral("true"));
    CloudServiceFactory::instance().saveSettings(
        service.get(), context.get());

    QCOMPARE(
        appsettings->cvalue(athlete->cyclist, athleteKey).toString(),
        QStringLiteral("gui-athlete-value"));
    QCOMPARE(
        appsettings->value(nullptr, globalKey).toString(),
        QStringLiteral("gui-global-value"));
    QCOMPARE(
        appsettings->cvalue(
            athlete->cyclist,
            service->syncOnStartupSettingName()).toString(),
        QStringLiteral("false"));
    QCOMPARE(
        appsettings->cvalue(
            athlete->cyclist,
            service->syncOnImportSettingName()).toString(),
        QStringLiteral("true"));
    QCOMPARE(athleteMigrationSettingsCrossThreadWrites(), 0);

    service.reset();
    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();
}

void TestAthleteMigrationSafety::guiCloudSettingsSaveClearsCustomUrl()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("CloudGuiClearUrl")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
        athlete->cyclist, TestCloudCompletionMode::Queued, 1, 500);
    const QString urlKey =
            QStringLiteral("<athlete-private>controlled/url");
    appsettings->setCValue(
        athlete->cyclist, urlKey,
        QStringLiteral("https://custom.invalid"));
    std::unique_ptr<CloudService> service(
        CloudServiceFactory::instance().newService(
            QStringLiteral("ControlledLifecycleCloud"), context.get()));
    QVERIFY(service);
    QCOMPARE(
        service->getSetting(urlKey, QString()).toString(),
        QStringLiteral("https://custom.invalid"));

    service->setSetting(urlKey, QString());
    CloudServiceFactory::instance().saveSettings(
        service.get(), context.get());
    const QString storedUrl = appsettings->cvalue(
        athlete->cyclist, urlKey,
        QStringLiteral("missing")).toString();

    service.reset();
    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QCOMPARE(storedUrl, QString());
}

void TestAthleteMigrationSafety::workerCloudSettingsClearUrlUsesDefault()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
        athleteDir, QStringLiteral("CloudWorkerClearUrl")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
        athlete->cyclist,
        TestCloudCompletionMode::QueuedClearsUrlTwice,
        1,
        500);
    const QString urlKey =
            QStringLiteral("<athlete-private>controlled/url");
    const QString threadKey =
            QStringLiteral("<athlete-private>controlled/thread");
    appsettings->setCValue(
        athlete->cyclist, urlKey,
        QStringLiteral("https://custom.invalid"));
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    athlete->cloudAutoDownload->checkDownload();

    const bool completionEmitted =
            waitForControlledCloudCompletions(1, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool finishedDelivered = waitUntil(
        [&finished]() { return finished.count() == 1; }, 2000);
    const QString storedUrl = appsettings->cvalue(
        athlete->cyclist, urlKey,
        QStringLiteral("missing")).toString();
    const QString storedThread = appsettings->cvalue(
        athlete->cyclist, threadKey,
        QStringLiteral("missing")).toString();
    const int parsedResults = athleteMigrationRideFileOpenCalls();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(completionEmitted);
    QVERIFY(providerDestroyed);
    QVERIFY(finishedDelivered);
    QCOMPARE(storedUrl, QString());
    QCOMPARE(storedThread, QStringLiteral("worker-value-after-url-clear"));
    QCOMPARE(parsedResults, 1);
}

void TestAthleteMigrationSafety::
defaultCloudUrlDoesNotInvalidatePayload()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudDefaultUrl")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Queued, 1, 500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    athlete->cloudAutoDownload->checkDownload();

    const bool completionEmitted =
            waitForControlledCloudCompletions(1, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool finishedDelivered = waitUntil(
            [&finished]() { return finished.count() == 1; }, 2000);
    const QString storedUrl = appsettings->cvalue(
        athlete->cyclist,
        QStringLiteral("<athlete-private>controlled/url"),
        QStringLiteral("missing")).toString();
    const int parsedResults = athleteMigrationRideFileOpenCalls();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(completionEmitted);
    QVERIFY(providerDestroyed);
    QVERIFY(finishedDelivered);
    QCOMPARE(storedUrl, QStringLiteral("missing"));
    QCOMPARE(parsedResults, 1);
}

void TestAthleteMigrationSafety::emptyCloudUrlUsesDefault()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudEmptyUrl")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    const QString urlKey =
            QStringLiteral("<athlete-private>controlled/url");
    appsettings->setCValue(athlete->cyclist, urlKey, QString());
    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Queued, 1, 500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    athlete->cloudAutoDownload->checkDownload();

    const bool completionEmitted =
            waitForControlledCloudCompletions(1, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool finishedDelivered = waitUntil(
            [&finished]() { return finished.count() == 1; }, 2000);
    const QString storedUrl = appsettings->cvalue(
        athlete->cyclist, urlKey, QStringLiteral("missing")).toString();
    const int parsedResults = athleteMigrationRideFileOpenCalls();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(completionEmitted);
    QVERIFY(providerDestroyed);
    QVERIFY(finishedDelivered);
    QCOMPARE(storedUrl, QString());
    QCOMPARE(parsedResults, 1);
}

void TestAthleteMigrationSafety::changedCloudUrlInvalidatesPayload()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudChangedUrl")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    const QString urlKey =
            QStringLiteral("<athlete-private>controlled/url");
    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Queued, 1, 500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    athlete->cloudAutoDownload->checkDownload();

    const bool completionEmitted =
            waitForControlledCloudCompletions(1, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    appsettings->setCValue(
        athlete->cyclist, urlKey,
        QStringLiteral("https://changed.invalid"));
    const bool finishedDelivered = waitUntil(
            [&finished]() { return finished.count() == 1; }, 2000);
    const QString storedUrl = appsettings->cvalue(
        athlete->cyclist, urlKey).toString();
    const int parsedResults = athleteMigrationRideFileOpenCalls();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(completionEmitted);
    QVERIFY(providerDestroyed);
    QVERIFY(finishedDelivered);
    QCOMPARE(storedUrl, QStringLiteral("https://changed.invalid"));
    QCOMPARE(parsedResults, 0);
}

void TestAthleteMigrationSafety::
queuedCloudPayloadIsDiscardedWhenStartupSyncIsDisabled()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudSyncDisabled")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Queued, 1, 500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    athlete->cloudAutoDownload->checkDownload();

    const bool completionEmitted =
            waitForControlledCloudCompletions(1, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    disableControlledCloudAutoDownload(athlete->cyclist);
    const bool finishedDelivered = waitUntil(
            [&finished]() { return finished.count() == 1; }, 2000);
    const QString syncValue = appsettings->cvalue(
        athlete->cyclist,
        CloudServiceFactory::instance()
            .service(QStringLiteral("ControlledLifecycleCloud"))
            ->syncOnStartupSettingName()).toString();
    const int parsedResults = athleteMigrationRideFileOpenCalls();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(completionEmitted);
    QVERIFY(providerDestroyed);
    QVERIFY(finishedDelivered);
    QCOMPARE(syncValue, QStringLiteral("false"));
    QCOMPARE(parsedResults, 0);
}

void TestAthleteMigrationSafety::
progressCallbackCanDestroyDownloader()
{
    constexpr int EntryCount = 3;

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudProgressDestruction")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Queued,
            EntryCount, 500);
    QPointer<CloudServiceAutoDownload> worker(
        athlete->cloudAutoDownload);
    bool destroyedFromProgress = false;
    connect(
        context.get(), &Context::autoDownloadProgress,
        context.get(), [&]() {
            if (destroyedFromProgress) return;
            destroyedFromProgress = true;
            athleteStorage.destroy();
        });

    worker->checkDownload();
    const bool allReadsStarted =
            waitForControlledCloudReads(EntryCount, 5000);
    const bool allCompletionsEmitted =
            waitForControlledCloudCompletions(EntryCount, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool callbackRan = waitUntil(
            [&]() { return destroyedFromProgress; }, 2000);

    if (athleteStorage.constructed()) athleteStorage.destroy();
    const int buffersOutstanding =
            cloudAutoDownloadTestBuffersOutstanding();
    const int parsedResults = athleteMigrationRideFileOpenCalls();
    cleanupControlledCloudAutoDownload();

    QVERIFY(allReadsStarted);
    QVERIFY(allCompletionsEmitted);
    QVERIFY(providerDestroyed);
    QVERIFY(callbackRan);
    QVERIFY(worker.isNull());
    QCOMPARE(parsedResults, 0);
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::timedOutCloudReadReleasesBuffer()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudTimedOutRead")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Never, 8, 25);
    enableSuccessfulFollowUpCloudAutoDownload(athlete->cyclist);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    const bool spyValid = finished.isValid();
    athlete->cloudAutoDownload->checkDownload();

    const bool readStarted = waitForControlledCloudReads(1, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool additionalReadStarted =
            waitForControlledCloudReads(1, 50);
    const bool finishedDelivered = waitUntil(
            [&finished]() { return finished.count() == 1; }, 1000);
    const bool followUpParsed = waitUntil(
            []() { return athleteMigrationRideFileOpenCalls() == 1; },
            1000);
    const QList<QByteArray> parsedPayloads =
            athleteMigrationRideFilePayloads();
    const int parsedResults = athleteMigrationRideFileOpenCalls();
    const int buffersAllocated = cloudAutoDownloadTestBuffersAllocated();
    const int buffersReleased = cloudAutoDownloadTestBuffersReleased();
    const int buffersOutstanding = cloudAutoDownloadTestBuffersOutstanding();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(spyValid);
    QVERIFY2(readStarted, "timed-out cloud read did not start");
    QVERIFY2(!additionalReadStarted,
             "provider continued after its first request timed out");
    QVERIFY2(providerDestroyed,
             "timed-out cloud provider was not cleaned up");
    QVERIFY2(finishedDelivered,
             "timed-out cloud download did not report completion");
    QVERIFY2(followUpParsed,
             "a healthy provider did not continue after another timed out");
    QCOMPARE(parsedResults, 1);
    QCOMPARE(parsedPayloads.size(), 1);
    QCOMPARE(parsedPayloads.first(), QByteArray("follow-up-payload"));
    QCOMPARE(buffersAllocated, 2);
    QCOMPARE(buffersReleased, 2);
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::rejectedCloudReadReleasesBufferPromptly()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudRejectedRead")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Reject, 1, 500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    const bool spyValid = finished.isValid();
    QElapsedTimer timer;
    timer.start();
    athlete->cloudAutoDownload->checkDownload();

    const bool readStarted = waitForControlledCloudReads(1, 5000);
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool finishedPromptly = waitUntil(
            [&finished]() { return finished.count() == 1; }, 250);
    const qint64 elapsedMs = timer.elapsed();
    const int parsedResults = athleteMigrationRideFileOpenCalls();
    const int buffersAllocated = cloudAutoDownloadTestBuffersAllocated();
    const int buffersReleased = cloudAutoDownloadTestBuffersReleased();
    const int buffersOutstanding = cloudAutoDownloadTestBuffersOutstanding();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(spyValid);
    QVERIFY2(readStarted, "rejected cloud read did not start");
    QVERIFY2(providerDestroyed,
             "provider for rejected cloud read was not cleaned up");
    QVERIFY2(finishedPromptly,
             "rejected cloud read waited for the request timeout");
    QVERIFY2(elapsedMs < 250,
             "rejected cloud read did not finish promptly");
    QCOMPARE(parsedResults, 0);
    QCOMPARE(buffersAllocated, 1);
    QCOMPARE(buffersReleased, 1);
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::
duplicateCloudCompletionIsAppliedOnceAcrossRepeatedRuns()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudDuplicateCompletion")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Duplicate, 1, 500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    const bool spyValid = finished.isValid();

    athlete->cloudAutoDownload->checkDownload();
    const bool firstRead = waitForControlledCloudReads(1, 5000);
    const bool firstCompletions =
            waitForControlledCloudCompletions(2, 5000);
    const bool firstProviderDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool firstFinished = waitUntil(
            [&finished]() { return finished.count() == 1; }, 1000);

    athlete->cloudAutoDownload->checkDownload();
    const bool secondRead = waitForControlledCloudReads(1, 5000);
    const bool secondCompletions =
            waitForControlledCloudCompletions(2, 5000);
    const bool secondProviderDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool secondFinished = waitUntil(
            [&finished]() { return finished.count() == 2; }, 1000);

    const int parsedResults = athleteMigrationRideFileOpenCalls();
    const int crossThreadProviderAccesses =
            controlledCloudCrossThreadProviderAccesses();
    const int destroyedProviderAccesses =
            controlledCloudDestroyedProviderAccesses();
    const int buffersAllocated = cloudAutoDownloadTestBuffersAllocated();
    const int buffersReleased = cloudAutoDownloadTestBuffersReleased();
    const int buffersOutstanding = cloudAutoDownloadTestBuffersOutstanding();
    const QList<QByteArray> parsedPayloads =
            athleteMigrationRideFilePayloads();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(spyValid);
    QVERIFY(firstRead);
    QVERIFY(firstCompletions);
    QVERIFY(firstProviderDestroyed);
    QVERIFY(firstFinished);
    QVERIFY(secondRead);
    QVERIFY(secondCompletions);
    QVERIFY(secondProviderDestroyed);
    QVERIFY(secondFinished);
    QCOMPARE(parsedResults, 2);
    QCOMPARE(crossThreadProviderAccesses, 0);
    QCOMPARE(destroyedProviderAccesses, 0);
    QCOMPARE(parsedPayloads.size(), 2);
    for (const QByteArray &payload : parsedPayloads) {
        QCOMPARE(payload, QByteArray("not-a-valid-fit"));
    }
    QCOMPARE(buffersAllocated, 2);
    QCOMPARE(buffersReleased, 2);
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::
providerCancellationDoesNotDestroyActiveCall_data()
{
    QTest::addColumn<int>("completionMode");
    QTest::newRow("open")
            << int(TestCloudCompletionMode::BlockInOpen);
    QTest::newRow("directory")
            << int(TestCloudCompletionMode::BlockInDirectory);
    QTest::newRow("read")
            << int(TestCloudCompletionMode::BlockInRead);
    QTest::newRow("base-network-reply")
            << int(TestCloudCompletionMode::BlockInBaseAbort);
    QTest::newRow("completion-callback")
            << int(TestCloudCompletionMode::BlockInCompletion);
}

void TestAthleteMigrationSafety::
providerCancellationDoesNotDestroyActiveCall()
{
    QFETCH(int, completionMode);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudNestedCancellation")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist,
            TestCloudCompletionMode(completionMode),
            1,
            30000);
    CloudServiceAutoDownload *worker = athlete->cloudAutoDownload;
    worker->checkDownload();

    const bool callBlocked = waitForControlledCloudBlockedCall(5000);
    QElapsedTimer timer;
    timer.start();
    worker->cancelAndWait();
    const qint64 cancelElapsedMs = timer.elapsed();
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(1000);
    const bool workerStopped = !worker->isRunning();
    const int destroyedDuringCall =
            controlledCloudDestroyedDuringCall();
    const int abortCalls = controlledCloudAbortCalls();
    const int baseReplyAbortCalls =
            controlledCloudBaseReplyAbortCalls();
    const int crossThreadProviderAccesses =
            controlledCloudCrossThreadProviderAccesses();
    const int buffersOutstanding =
            cloudAutoDownloadTestBuffersOutstanding();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY2(callBlocked, "provider did not enter its nested event loop");
    QVERIFY2(workerStopped, "cancelled cloud worker remained running");
    QVERIFY2(cancelElapsedMs < 2000,
             "nested provider call did not cancel promptly");
    QVERIFY2(providerDestroyed,
             "cancelled provider was not destroyed after its call returned");
    QCOMPARE(destroyedDuringCall, 0);
    QVERIFY(abortCalls >= 1);
    if (TestCloudCompletionMode(completionMode)
            == TestCloudCompletionMode::BlockInBaseAbort) {
        QVERIFY(baseReplyAbortCalls >= 1);
    }
    QCOMPARE(crossThreadProviderAccesses, 0);
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::cancellationFromStartSignalStopsSafely()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudCancelFromStart")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist, TestCloudCompletionMode::Inline, 1, 500);
    CloudServiceAutoDownload *worker = athlete->cloudAutoDownload;
    QSignalSpy started(context.get(), &Context::autoDownloadStart);
    const bool spyValid = started.isValid();
    connect(
        context.get(), &Context::autoDownloadStart,
        worker, [worker]() { worker->cancelAndWait(); },
        Qt::DirectConnection);

    worker->checkDownload();
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(1000);
    const bool workerStopped = !worker->isRunning();
    const int buffersOutstanding =
            cloudAutoDownloadTestBuffersOutstanding();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(spyValid);
    QCOMPARE(started.count(), 1);
    QVERIFY(workerStopped);
    QVERIFY(providerDestroyed);
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::
staleQueuedResultsAreDiscardedAfterRestart()
{
    constexpr int EntryCount = 2;

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudStaleGeneration")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);
    CloudServiceAutoDownload *worker = athlete->cloudAutoDownload;

    configureControlledCloudAutoDownload(
            athlete->cyclist,
            TestCloudCompletionMode::Queued,
            EntryCount,
            500);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    QSignalSpy progress(context.get(), &Context::autoDownloadProgress);
    const bool spyValid = finished.isValid() && progress.isValid();

    worker->checkDownload();
    const bool firstReads =
            waitForControlledCloudReads(EntryCount, 5000);
    const bool firstCompletions =
            waitForControlledCloudCompletions(EntryCount, 5000);
    const bool firstProviderDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    worker->cancelAndWait();
    const int parsedBeforeRestart = athleteMigrationRideFileOpenCalls();

    worker->checkDownload();
    const bool secondReads =
            waitForControlledCloudReads(EntryCount, 5000);
    const bool secondCompletions =
            waitForControlledCloudCompletions(EntryCount, 5000);
    const bool secondProviderDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool secondRunDelivered = waitUntil(
            [&finished, &progress]() {
                return finished.count() == 1
                    && progress.count() == EntryCount + 1
                    && athleteMigrationRideFileOpenCalls() == EntryCount;
            },
            2000);

    const int parsedResults = athleteMigrationRideFileOpenCalls();
    const int buffersAllocated = cloudAutoDownloadTestBuffersAllocated();
    const int buffersReleased = cloudAutoDownloadTestBuffersReleased();
    const int buffersOutstanding = cloudAutoDownloadTestBuffersOutstanding();

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(spyValid);
    QVERIFY(firstReads);
    QVERIFY(firstCompletions);
    QVERIFY(firstProviderDestroyed);
    QCOMPARE(parsedBeforeRestart, 0);
    QVERIFY(secondReads);
    QVERIFY(secondCompletions);
    QVERIFY(secondProviderDestroyed);
    QVERIFY(secondRunDelivered);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(parsedResults, EntryCount);
    QCOMPARE(progress.count(), EntryCount + 1);
    for (int i = 0; i <= EntryCount; ++i) {
        const QList<QVariant> values = progress.at(i);
        QCOMPARE(values.size(), 4);
        QCOMPARE(values.at(2).toInt(), i);
        QCOMPARE(values.at(3).toInt(), EntryCount);
    }
    QCOMPARE(buffersAllocated, EntryCount * 2);
    QCOMPARE(buffersReleased, EntryCount * 2);
    QCOMPARE(buffersOutstanding, 0);
}

void TestAthleteMigrationSafety::
legacyReadCompleteDoesNotTakeCallerOwnership()
{
    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    CloudServiceAutoDownload receiver(context.get());
    QByteArray callerOwned("caller-owned");

    receiver.readComplete(
        &callerOwned, QStringLiteral("legacy.fit"), QString());

    QCOMPARE(callerOwned, QByteArray("caller-owned"));
}

void TestAthleteMigrationSafety::
noEnabledCloudServiceEmitsNoLifecycleSignals()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudNoEnabledService")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);
    QSignalSpy started(context.get(), &Context::autoDownloadStart);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);

    athlete->cloudAutoDownload->checkDownload();
    QCoreApplication::processEvents();

    const bool spiesValid = started.isValid() && finished.isValid();
    const bool workerRunning = athlete->cloudAutoDownload->isRunning();
    athleteStorage.destroy();

    QVERIFY(spiesValid);
    QVERIFY(!workerRunning);
    QCOMPARE(started.count(), 0);
    QCOMPARE(finished.count(), 0);
}

void TestAthleteMigrationSafety::
autoDownloadEndFollowsImportsAndProgress()
{
    constexpr int EntryCount = 3;

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudEventOrdering")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    Athlete *athlete = athleteStorage.construct(context.get(), athleteDir);

    configureControlledCloudAutoDownload(
            athlete->cyclist,
            TestCloudCompletionMode::Queued,
            EntryCount,
            500);
    QSignalSpy started(context.get(), &Context::autoDownloadStart);
    QSignalSpy finished(context.get(), &Context::autoDownloadEnd);
    QSignalSpy progress(context.get(), &Context::autoDownloadProgress);
    const bool spiesValid =
            started.isValid() && finished.isValid() && progress.isValid();
    QStringList eventOrder;
    connect(
        context.get(), &Context::autoDownloadStart,
        context.get(), [&eventOrder]() {
            eventOrder.append(QStringLiteral("start"));
        });
    connect(
        context.get(), &Context::autoDownloadProgress,
        context.get(),
        [&eventOrder](const QString &, double, int current, int) {
            eventOrder.append(
                QStringLiteral("progress:%1").arg(current));
        });
    bool endFollowedImports = false;
    connect(
        context.get(), &Context::autoDownloadEnd,
        context.get(), [&endFollowedImports, &eventOrder]() {
            eventOrder.append(QStringLiteral("end"));
            endFollowedImports =
                    athleteMigrationRideFileOpenCalls() == EntryCount;
        });

    athlete->cloudAutoDownload->checkDownload();
    const bool providerDestroyed =
            waitForControlledCloudProviderDestroyed(5000);
    const bool eventsDelivered = waitUntil(
            [&finished, &progress]() {
                return finished.count() == 1
                    && progress.count() == EntryCount + 1;
            },
            2000);

    athleteStorage.destroy();
    cleanupControlledCloudAutoDownload();

    QVERIFY(spiesValid);
    QVERIFY(providerDestroyed);
    QVERIFY(eventsDelivered);
    QCOMPARE(started.count(), 1);
    QVERIFY(endFollowedImports);
    QCOMPARE(
        eventOrder,
        QStringList({
            QStringLiteral("start"),
            QStringLiteral("progress:0"),
            QStringLiteral("progress:1"),
            QStringLiteral("progress:2"),
            QStringLiteral("progress:3"),
            QStringLiteral("end")}));
    QCOMPARE(progress.count(), EntryCount + 1);
    for (int i = 0; i <= EntryCount; ++i) {
        const QList<QVariant> values = progress.at(i);
        QCOMPARE(values.size(), 4);
        QCOMPARE(values.at(0).toString(),
                 QStringLiteral("Controlled lifecycle cloud"));
        QCOMPARE(
            values.at(1).toDouble(),
            (100.0 * double(i)) / double(EntryCount));
        QCOMPARE(values.at(2).toInt(), i);
        QCOMPARE(values.at(3).toInt(), EntryCount);
    }
}

void TestAthleteMigrationSafety::plainQtThreadLifecycleIsJoined()
{
    EmptyQtThread thread;
    thread.start();
    QVERIFY(thread.wait(5000));
}

void TestAthleteMigrationSafety::baseCloudAbortStopsRunningReplies()
{
    AbortProbeService service;
    auto *reply = new AbortProbeReply(&service);

    QVERIFY(reply->isRunning());
    service.abortRequests();
    QCOMPARE(reply->abortCalls(), 1);
    QVERIFY(!reply->isRunning());

    service.abortRequests();
    QCOMPARE(reply->abortCalls(), 1);
}

void TestAthleteMigrationSafety::cloudCompressionModesAreExtracted()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDir athleteDir(root.path());
    QVERIFY(createStructuredAthlete(
            athleteDir, QStringLiteral("CloudCompression")));
    configureAthlete(athleteDir, VERSION_LATEST, true);

    std::unique_ptr<Context> context(createAthleteMigrationTestContext());
    SeededAthleteStorage athleteStorage;
    athleteStorage.construct(context.get(), athleteDir);

    const QByteArray payload =
            QByteArray("payload-for-cloud-compression-").repeated(128);
    const QByteArray zipped = zipPayload(payload);
    const QByteArray gzipped = gzipPayload(payload);
    QVERIFY(!zipped.isEmpty());
    QVERIFY(!gzipped.isEmpty());

    const QString name = QStringLiteral("2026_07_08_12_00_00.fit");
    QStringList errors;
    QVERIFY(!CloudService::uncompressRide(
        context.get(), CloudService::none, payload, name, errors));
    QVERIFY(errors.isEmpty());
    QVERIFY(!CloudService::uncompressRide(
        context.get(), CloudService::zip, zipped,
        name + QStringLiteral(".zip"), errors));
    QVERIFY(errors.isEmpty());
    QVERIFY(!CloudService::uncompressRide(
        context.get(), CloudService::gzip, gzipped,
        name + QStringLiteral(".gz"), errors));
    QVERIFY(errors.isEmpty());

    QStringList mismatchErrors;
    QVERIFY(!CloudService::uncompressRide(
        context.get(), CloudService::zip, zipped, name, mismatchErrors));
    QVERIFY(!mismatchErrors.isEmpty());

    const QList<QByteArray> extracted =
            athleteMigrationRideFilePayloads();
    athleteStorage.destroy();

    QCOMPARE(athleteMigrationRideFileOpenCalls(), 3);
    QCOMPARE(extracted.size(), 3);
    QCOMPARE(extracted.at(0), payload);
    QCOMPARE(extracted.at(1), payload);
    QCOMPARE(extracted.at(2), payload);
}

void TestAthleteMigrationSafety::nolioTokenRefreshIsSingleFlight()
{
    std::atomic<int> operationCalls{0};
    QSemaphore operationStarted;
    QSemaphore releaseOperation;
    const QString inputToken =
            QStringLiteral("unit-test-nolio-refresh-token");
    NolioRefreshThread first(
        inputToken, operationCalls, operationStarted, releaseOperation);
    NolioRefreshThread second(
        inputToken, operationCalls, operationStarted, releaseOperation);

    first.start();
    const bool firstStarted = operationStarted.tryAcquire(1, 2000);
    second.start();
    QTest::qWait(50);
    releaseOperation.release(2);
    const bool firstJoined = first.wait(2000);
    const bool secondJoined = second.wait(2000);
    const NolioTokenRefreshResult firstResult = first.result();
    const NolioTokenRefreshResult secondResult = second.result();
    const NolioTokenRefreshResult cachedResult =
            NolioTokenRefreshCoordinator::refresh(
        inputToken,
        [&operationCalls]() {
            operationCalls.fetch_add(1, std::memory_order_relaxed);
            return NolioTokenRefreshResult();
        });

    QVERIFY(firstStarted);
    QVERIFY(firstJoined);
    QVERIFY(secondJoined);
    QCOMPARE(operationCalls.load(std::memory_order_relaxed), 1);
    QVERIFY(firstResult.success);
    QVERIFY(secondResult.success);
    QCOMPARE(firstResult.accessToken, secondResult.accessToken);
    QCOMPARE(firstResult.refreshToken, secondResult.refreshToken);
    QCOMPARE(firstResult.refreshedAt, secondResult.refreshedAt);
    QVERIFY(cachedResult.success);
    QCOMPARE(cachedResult.accessToken, firstResult.accessToken);
    QCOMPARE(cachedResult.refreshToken, firstResult.refreshToken);
}

void TestAthleteMigrationSafety::nolioTokenRefreshCacheExpires()
{
    int operationCalls = 0;
    const QString inputToken =
            QStringLiteral("unit-test-nolio-expiring-cache-token");
    const auto operation = [&operationCalls, &inputToken]() {
        ++operationCalls;
        NolioTokenRefreshResult result;
        result.success = true;
        result.accessToken = QStringLiteral("access-%1")
                .arg(operationCalls);
        result.refreshToken = inputToken;
        result.refreshedAt = QStringLiteral("refresh-%1")
                .arg(operationCalls);
        return result;
    };

    const NolioTokenRefreshResult first =
            NolioTokenRefreshCoordinator::refresh(
        inputToken, operation, {},
        std::chrono::milliseconds(1));
    QTest::qWait(10);
    const NolioTokenRefreshResult second =
            NolioTokenRefreshCoordinator::refresh(
        inputToken, operation, {},
        std::chrono::milliseconds(1));

    QVERIFY(first.success);
    QVERIFY(second.success);
    QCOMPARE(operationCalls, 2);
    QCOMPARE(first.accessToken, QStringLiteral("access-1"));
    QCOMPARE(second.accessToken, QStringLiteral("access-2"));
}

void
TestAthleteMigrationSafety::nolioTokenRefreshFollowerCanCancel()
{
    std::atomic<int> operationCalls{0};
    QSemaphore operationStarted;
    QSemaphore releaseOperation;
    const QString inputToken =
            QStringLiteral("unit-test-nolio-cancel-token");
    NolioRefreshThread leader(
        inputToken, operationCalls, operationStarted, releaseOperation);
    NolioRefreshThread follower(
        inputToken, operationCalls, operationStarted, releaseOperation);

    leader.start();
    const bool leaderStarted = operationStarted.tryAcquire(1, 2000);
    follower.start();
    QTest::qWait(50);
    follower.requestInterruption();
    const bool followerStoppedPromptly = follower.wait(1000);

    releaseOperation.release(2);
    const bool leaderJoined = leader.wait(2000);
    const bool followerJoined =
            followerStoppedPromptly || follower.wait(2000);
    const NolioTokenRefreshResult leaderResult = leader.result();
    const NolioTokenRefreshResult followerResult = follower.result();

    QVERIFY(leaderStarted);
    QVERIFY2(followerStoppedPromptly,
             "Nolio refresh follower ignored cancellation");
    QVERIFY(leaderJoined);
    QVERIFY(followerJoined);
    QCOMPARE(operationCalls.load(std::memory_order_relaxed), 1);
    QVERIFY(leaderResult.success);
    QVERIFY(!followerResult.success);
    QVERIFY(followerResult.error.contains(
        QStringLiteral("cancel"), Qt::CaseInsensitive));
}

void TestAthleteMigrationSafety::networkReplyWaitTimesOut()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    const QString endpoint = QStringLiteral("http://127.0.0.1:%1/token")
            .arg(server.serverPort());
    QNetworkAccessManager manager;
    QNetworkReply *reply = manager.post(
        QNetworkRequest(QUrl(endpoint)), QByteArray());

    QElapsedTimer timer;
    timer.start();
    const NetworkReplyWaitResult result =
            waitForNetworkReply(reply, 30);
    const qint64 elapsedMs = timer.elapsed();
    const bool connected = server.hasPendingConnections();
    QTcpSocket *connection =
            connected ? server.nextPendingConnection() : nullptr;

    if (connection) {
        connection->abort();
        connection->deleteLater();
    }
    reply->deleteLater();

    QVERIFY2(connected,
             "network wait did not connect to the local test server");
    QVERIFY(result == NetworkReplyWaitResult::TimedOut);
    QVERIFY2(elapsedMs >= 15,
             "network wait timed out substantially too early");
    QVERIFY2(elapsedMs < 1000,
             "network wait did not respect its timeout");
}

void
TestAthleteMigrationSafety::
networkReplyWaitPrefersPreexistingInterruption()
{
    AbortProbeReply reply(nullptr);
    reply.finish();

    const NetworkReplyWaitResult result =
            waitForNetworkReply(
                &reply, 1000, []() { return true; });

    QVERIFY(result == NetworkReplyWaitResult::Interrupted);
    QCOMPARE(reply.abortCalls(), 1);
}

void TestAthleteMigrationSafety::oauthRefreshHandlesImmediateReply()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    QByteArray request;
    bool responded = false;
    connect(
        &server, &QTcpServer::newConnection, &server,
        [&server, &request, &responded]() {
            while (server.hasPendingConnections()) {
                QTcpSocket *connection = server.nextPendingConnection();
                connect(
                    connection, &QTcpSocket::readyRead, connection,
                    [connection, &request, &responded]() {
                        request.append(connection->readAll());
                        if (responded
                            || !request.contains("\r\n\r\n")) {
                            return;
                        }

                        const QByteArray body(
                            "{\"access_token\":\"new-access\","
                            "\"refresh_token\":\"new-refresh\","
                            "\"expires_in\":3600}");
                        const QByteArray response =
                            QByteArray("HTTP/1.1 200 OK\r\n"
                                       "Content-Type: application/json\r\n"
                                       "Content-Length: ")
                            + QByteArray::number(body.size())
                            + QByteArray("\r\nConnection: close\r\n\r\n")
                            + body;
                        connection->write(response);
                        connection->flush();
                        responded = true;
                    });
            }
        });

    const QString endpoint = QStringLiteral("http://127.0.0.1:%1/token")
            .arg(server.serverPort());
    OAuthRefreshThread worker(endpoint);
    worker.start();

    const bool completed = waitUntil(
        [&worker]() { return worker.isFinished(); }, 3000);
    if (!completed) worker.requestInterruption();
    const bool joined = worker.wait(completed ? 1000 : 5000);
    const OAuthRefreshThread::Result result = worker.result();

    QVERIFY2(joined, "OAuth refresh thread did not stop during cleanup");
    QVERIFY2(completed, "OAuth refresh did not finish promptly");
    QVERIFY(responded);
    QVERIFY(result.succeeded);
    QCOMPARE(result.error, QString());
    QCOMPARE(result.accessToken, QStringLiteral("new-access"));
    QCOMPARE(result.refreshToken, QStringLiteral("new-refresh"));
    QCOMPARE(result.expiresIn, 3600);
}

void TestAthleteMigrationSafety::oauthRefreshTimesOut()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    const QString endpoint = QStringLiteral("http://127.0.0.1:%1/token")
            .arg(server.serverPort());
    OAuthRefreshThread worker(endpoint, 30);

    QElapsedTimer timer;
    timer.start();
    worker.start();

    const bool connected = waitUntil(
        [&server]() { return server.hasPendingConnections(); }, 1000);
    QTcpSocket *connection =
            connected ? server.nextPendingConnection() : nullptr;
    const bool completed = waitUntil(
        [&worker]() { return worker.isFinished(); }, 1000);
    const qint64 elapsedMs = timer.elapsed();

    if (!completed) worker.requestInterruption();
    const bool joined = worker.wait(completed ? 1000 : 5000);
    const OAuthRefreshThread::Result result = worker.result();

    if (connection) {
        connection->abort();
        connection->deleteLater();
    }

    QVERIFY2(joined, "OAuth refresh thread did not stop during cleanup");
    QVERIFY2(connected,
             "OAuth refresh did not connect to the local test server");
    QVERIFY2(completed, "OAuth refresh did not respect its timeout");
    QVERIFY(elapsedMs < 1000);
    QVERIFY(!result.succeeded);
    QVERIFY(result.error.contains(
        QStringLiteral("timed out"), Qt::CaseInsensitive));
}

void TestAthleteMigrationSafety::oauthRefreshHonorsThreadInterruption()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    const QString endpoint = QStringLiteral("http://127.0.0.1:%1/token")
            .arg(server.serverPort());
    OAuthRefreshThread worker(endpoint);
    worker.start();

    const bool connected = waitUntil(
        [&server]() { return server.hasPendingConnections(); }, 2000);
    QTcpSocket *connection =
            connected ? server.nextPendingConnection() : nullptr;

    QElapsedTimer timer;
    timer.start();
    worker.requestInterruption();
    const bool stoppedPromptly = worker.wait(1000);
    const qint64 stopElapsedMs = timer.elapsed();

    if (!stoppedPromptly) {
        if (connection) connection->abort();
        server.close();
    }
    const bool cleanupStopped =
            stoppedPromptly || worker.wait(5000);
    const OAuthRefreshThread::Result result = worker.result();

    QVERIFY2(cleanupStopped,
             "OAuth refresh thread did not stop during cleanup");
    QVERIFY2(connected,
             "OAuth refresh did not connect to the local test server");
    QVERIFY2(stoppedPromptly,
             "OAuth refresh ignored QThread interruption");
    QVERIFY(stopElapsedMs < 1000);
    QVERIFY(!result.succeeded);
    QVERIFY(result.error.contains(
        QStringLiteral("cancel"), Qt::CaseInsensitive));
}

void TestAthleteMigrationSafety::sslWarningsAreMarshaledToGuiThread()
{
    bool warningHandled = false;
    bool shownOnGuiThread = false;
    const std::shared_ptr<GuiMessageResponseState> response =
            answerNextMessageBoxOnGuiThread(
                warningHandled, shownOnGuiThread);

    SslWarningThread worker;
    worker.start();
    const bool completed = waitUntil(
        [&warningHandled]() { return warningHandled; },
        2000);
    response->active = false;
    worker.release();
    const bool joined = worker.wait(5000);

    QVERIFY(completed);
    QVERIFY(joined);
    QVERIFY(warningHandled);
    QVERIFY(shownOnGuiThread);
}

QTEST_MAIN(TestAthleteMigrationSafety)
#include "testAthleteMigrationSafety.moc"
