#include "Cloud/OAuthTokenReplyController.h"

#include <QCoreApplication>
#include <QEvent>
#include <QNetworkReply>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>

class ControlledReply final : public QNetworkReply
{
public:
    ControlledReply()
    {
        setOpenMode(QIODevice::ReadOnly);
    }

    void abort() override
    {
        ++abortCalls;
        if (isFinished()) return;
        setError(
            QNetworkReply::OperationCanceledError,
            QStringLiteral("Synthetic cancellation"));
        setFinished(true);
        emit finished();
    }

    void finishNormally()
    {
        if (isFinished()) return;
        setFinished(true);
        emit finished();
    }

    int abortCalls = 0;

protected:
    qint64 readData(char *, qint64) override
    {
        return -1;
    }
};

namespace {

void processDeferredDeletes()
{
    QCoreApplication::sendPostedEvents(
        nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

} // namespace

class TestOAuthTokenReplyController : public QObject
{
    Q_OBJECT

private slots:
    void rejectsInvalidStartInputs();
    void normalCompletionStopsDeadline();
    void timeoutAbortsAndClassifiesReply();
    void cancellationAbortsWithoutBecomingTimeout();
    void rejectsOverlappingReply();
    void untrackedReplyIsStillDeleted();
    void repeatedRepliesLeaveNoObjects();
};

void TestOAuthTokenReplyController::rejectsInvalidStartInputs()
{
    OAuthTokenReplyController controller(this);
    QVERIFY(!controller.start(nullptr, 50));

    QPointer<ControlledReply> reply = new ControlledReply();
    QVERIFY(!controller.start(reply, 0));
    QVERIFY(
        controller.complete(reply)
        == OAuthTokenReplyController::Completion::Untracked);
    processDeferredDeletes();
    QVERIFY(reply.isNull());
}

void TestOAuthTokenReplyController::
normalCompletionStopsDeadline()
{
    OAuthTokenReplyController controller(this);
    QPointer<ControlledReply> reply = new ControlledReply();
    OAuthTokenReplyController::Completion completion =
        OAuthTokenReplyController::Completion::Untracked;
    connect(reply, &QNetworkReply::finished, this, [&] {
        completion = controller.complete(reply);
    });

    QVERIFY(controller.start(reply, 50));
    reply->finishNormally();
    QVERIFY(
        completion
        == OAuthTokenReplyController::Completion::Finished);
    QTest::qWait(80);
    processDeferredDeletes();
    QVERIFY(reply.isNull());
}

void TestOAuthTokenReplyController::
timeoutAbortsAndClassifiesReply()
{
    OAuthTokenReplyController controller(this);
    QPointer<ControlledReply> reply = new ControlledReply();
    QSignalSpy finished(reply, &QNetworkReply::finished);
    OAuthTokenReplyController::Completion completion =
        OAuthTokenReplyController::Completion::Untracked;
    connect(reply, &QNetworkReply::finished, this, [&] {
        completion = controller.complete(reply);
    });

    QVERIFY(controller.start(reply, 20));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 500);
    QVERIFY(
        completion
        == OAuthTokenReplyController::Completion::TimedOut);
    QCOMPARE(reply->abortCalls, 1);
    processDeferredDeletes();
    QVERIFY(reply.isNull());
}

void TestOAuthTokenReplyController::
cancellationAbortsWithoutBecomingTimeout()
{
    OAuthTokenReplyController controller(this);
    QPointer<ControlledReply> reply = new ControlledReply();
    OAuthTokenReplyController::Completion completion =
        OAuthTokenReplyController::Completion::Untracked;
    connect(reply, &QNetworkReply::finished, this, [&] {
        completion = controller.complete(reply);
    });

    QVERIFY(controller.start(reply, 50));
    controller.cancel();
    QVERIFY(
        completion
        == OAuthTokenReplyController::Completion::Cancelled);
    QCOMPARE(reply->abortCalls, 1);
    QTest::qWait(80);
    processDeferredDeletes();
    QVERIFY(reply.isNull());
}

void TestOAuthTokenReplyController::rejectsOverlappingReply()
{
    OAuthTokenReplyController controller(this);
    QPointer<ControlledReply> first = new ControlledReply();
    QPointer<ControlledReply> second = new ControlledReply();
    connect(first, &QNetworkReply::finished, this, [&] {
        controller.complete(first);
    });

    QVERIFY(controller.start(first, 1000));
    QVERIFY(!controller.start(second, 1000));
    controller.cancel();
    QVERIFY(
        controller.complete(second)
        == OAuthTokenReplyController::Completion::Untracked);
    processDeferredDeletes();
    QVERIFY(first.isNull());
    QVERIFY(second.isNull());
}

void TestOAuthTokenReplyController::
untrackedReplyIsStillDeleted()
{
    OAuthTokenReplyController controller(this);
    QPointer<ControlledReply> reply = new ControlledReply();

    QVERIFY(
        controller.complete(reply)
        == OAuthTokenReplyController::Completion::Untracked);
    processDeferredDeletes();
    QVERIFY(reply.isNull());
}

void TestOAuthTokenReplyController::repeatedRepliesLeaveNoObjects()
{
    OAuthTokenReplyController controller(this);
    QList<QPointer<ControlledReply>> replies;
    for (int index = 0; index < 100; ++index) {
        QPointer<ControlledReply> reply = new ControlledReply();
        replies.append(reply);
        connect(reply, &QNetworkReply::finished, this, [&controller, reply] {
            controller.complete(reply);
        });
        QVERIFY(controller.start(reply, 1000));
        reply->finishNormally();
    }

    processDeferredDeletes();
    for (const QPointer<ControlledReply> &reply : replies) {
        QVERIFY(reply.isNull());
    }
}

QTEST_GUILESS_MAIN(TestOAuthTokenReplyController)

#include "testOAuthTokenReplyController.moc"
