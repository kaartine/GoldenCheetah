/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Core/Athlete.h"
#include "Core/GcUpgrade.h"
#include "Core/Settings.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <array>
#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>

void resetAthleteMigrationTestSettings();
void setAthleteMigrationThrowOnIdWrite(bool enabled);
void setAthleteMigrationThrowOnRideCacheConstruction(bool enabled);
Context *createAthleteMigrationTestContext();

namespace {

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
    void publishedContextRollsBackLateConstructionFailure();
    void constructorFailureRollsBackPublishedContextAndOwners();
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

QTEST_MAIN(TestAthleteMigrationSafety)
#include "testAthleteMigrationSafety.moc"
