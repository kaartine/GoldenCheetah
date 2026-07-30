#include "Cloud/OpenDataCaptureStateMachine.h"

#include <QTest>

using namespace OpenDataCaptureStateMachine;

namespace {

struct Harness
{
    bool allowed = true;
    bool startupReady = true;
    qsizetype sourceCount = 0;
    qsizetype captureCalls = 0;
    QList<qsizetype> processedSources;
    qsizetype validationCalls = 0;
    qsizetype sealCalls = 0;
    qsizetype descriptionCalls = 0;
    qsizetype descriptionsBeforeComplete = 0;
    qsizetype handoffCalls = 0;
    bool captureSucceeds = true;
    bool validationSucceeds = true;
    bool sealSucceeds = true;

    Operations operations()
    {
        return {
            [this]() { return allowed; },
            [this]() { return startupReady; },
            [this](qsizetype &capturedCount, QString &error) {
                ++captureCalls;
                capturedCount = sourceCount;
                if (!captureSucceeds)
                    error = QStringLiteral("capture failed");
                return captureSucceeds;
            },
            [this](qsizetype index, QString &) {
                processedSources.append(index);
                return true;
            },
            [this](QString &error) {
                ++validationCalls;
                if (!validationSucceeds)
                    error = QStringLiteral("snapshot changed");
                return validationSucceeds;
            },
            [this](QString &error) {
                ++sealCalls;
                if (!sealSucceeds)
                    error = QStringLiteral("seal failed");
                return sealSucceeds;
            },
            [this](QString &) {
                ++descriptionCalls;
                return descriptionCalls
                        <= descriptionsBeforeComplete
                    ? DescriptionResult::InProgress
                    : DescriptionResult::Complete;
            },
            [this]() { ++handoffCalls; }
        };
    }
};

AdvanceResult runToTerminal(
    StateMachine &stateMachine,
    QString &error)
{
    for (int step = 0; step < 100; ++step) {
        const AdvanceResult result =
            stateMachine.advance(error);
        if (result != AdvanceResult::More)
            return result;
    }
    return AdvanceResult::Failed;
}

} // namespace

class TestOpenDataCaptureStateMachine : public QObject
{
    Q_OBJECT

private slots:
    void startupBarrierPreventsPartialCapture();
    void processesAtMostOneSourcePerAdvance();
    void validatesSnapshotBeforeSealing();
    void mutationFailurePreventsHandoff();
    void cancellationPreventsFurtherWork();
    void revokedConsentPreventsHandoff();
    void archiveDescriptionYieldsBetweenChunks();
};

void
TestOpenDataCaptureStateMachine::startupBarrierPreventsPartialCapture()
{
    Harness harness;
    harness.startupReady = false;
    harness.sourceCount = 1;
    StateMachine stateMachine(harness.operations());
    QString error;

    QCOMPARE(
        stateMachine.advance(error),
        AdvanceResult::Waiting);
    QCOMPARE(harness.captureCalls, 0);
    QCOMPARE(harness.handoffCalls, 0);

    harness.startupReady = true;
    QCOMPARE(
        stateMachine.advance(error),
        AdvanceResult::More);
    QCOMPARE(harness.captureCalls, 0);
    QCOMPARE(
        runToTerminal(stateMachine, error),
        AdvanceResult::Complete);
    QCOMPARE(harness.captureCalls, 1);
    QCOMPARE(harness.handoffCalls, 1);
}

void
TestOpenDataCaptureStateMachine::processesAtMostOneSourcePerAdvance()
{
    Harness harness;
    harness.sourceCount = 3;
    StateMachine stateMachine(harness.operations());
    QString error;

    QCOMPARE(stateMachine.advance(error), AdvanceResult::More);
    QCOMPARE(stateMachine.advance(error), AdvanceResult::More);
    QCOMPARE(harness.processedSources.size(), 0);

    QCOMPARE(stateMachine.advance(error), AdvanceResult::More);
    QCOMPARE(harness.processedSources, QList<qsizetype>{0});
    QCOMPARE(stateMachine.advance(error), AdvanceResult::More);
    QCOMPARE(harness.processedSources, (QList<qsizetype>{0, 1}));
    QCOMPARE(stateMachine.advance(error), AdvanceResult::More);
    QCOMPARE(
        harness.processedSources,
        (QList<qsizetype>{0, 1, 2}));
}

void
TestOpenDataCaptureStateMachine::validatesSnapshotBeforeSealing()
{
    Harness harness;
    harness.sourceCount = 0;
    StateMachine stateMachine(harness.operations());
    QString error;

    QCOMPARE(
        runToTerminal(stateMachine, error),
        AdvanceResult::Complete);
    QCOMPARE(harness.validationCalls, 1);
    QCOMPARE(harness.sealCalls, 1);
    QCOMPARE(harness.handoffCalls, 1);
}

void
TestOpenDataCaptureStateMachine::mutationFailurePreventsHandoff()
{
    Harness harness;
    harness.sourceCount = 1;
    harness.validationSucceeds = false;
    StateMachine stateMachine(harness.operations());
    QString error;

    QCOMPARE(
        runToTerminal(stateMachine, error),
        AdvanceResult::Failed);
    QCOMPARE(error, QStringLiteral("snapshot changed"));
    QCOMPARE(harness.validationCalls, 1);
    QCOMPARE(harness.sealCalls, 0);
    QCOMPARE(harness.handoffCalls, 0);
}

void
TestOpenDataCaptureStateMachine::cancellationPreventsFurtherWork()
{
    Harness harness;
    harness.sourceCount = 2;
    StateMachine stateMachine(harness.operations());
    QString error;

    QCOMPARE(stateMachine.advance(error), AdvanceResult::More);
    QCOMPARE(stateMachine.advance(error), AdvanceResult::More);
    stateMachine.requestCancellation();

    QCOMPARE(
        stateMachine.advance(error),
        AdvanceResult::Cancelled);
    QCOMPARE(harness.processedSources.size(), 0);
    QCOMPARE(harness.validationCalls, 0);
    QCOMPARE(harness.handoffCalls, 0);
}

void
TestOpenDataCaptureStateMachine::revokedConsentPreventsHandoff()
{
    Harness harness;
    harness.sourceCount = 0;
    StateMachine stateMachine(harness.operations());
    QString error;

    for (int step = 0; step < 5; ++step)
        QCOMPARE(stateMachine.advance(error), AdvanceResult::More);
    QCOMPARE(harness.descriptionCalls, 1);
    QCOMPARE(harness.handoffCalls, 0);

    harness.allowed = false;
    QCOMPARE(
        stateMachine.advance(error),
        AdvanceResult::Cancelled);
    QCOMPARE(harness.handoffCalls, 0);
}

void
TestOpenDataCaptureStateMachine::archiveDescriptionYieldsBetweenChunks()
{
    Harness harness;
    harness.sourceCount = 0;
    harness.descriptionsBeforeComplete = 3;
    StateMachine stateMachine(harness.operations());
    QString error;

    QCOMPARE(
        runToTerminal(stateMachine, error),
        AdvanceResult::Complete);
    QCOMPARE(harness.descriptionCalls, 4);
    QCOMPARE(harness.handoffCalls, 1);
}

QTEST_GUILESS_MAIN(TestOpenDataCaptureStateMachine)
#include "testOpenDataCaptureStateMachine.moc"
