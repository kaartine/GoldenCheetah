/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "Train/WebDownloadImportPolicy.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

using WebDownloadImportPolicy::Completion;
using WebDownloadImportPolicy::Decision;
using WebDownloadImportPolicy::Gate;
using WebDownloadImportPolicy::Request;
using WebDownloadImportPolicy::RequestAction;
using WebDownloadImportPolicy::UserDecision;

Q_DECLARE_METATYPE(RequestAction)

namespace {

constexpr quintptr OwningPage = 0x101;
constexpr quintptr ForeignPage = 0x202;

Request validRequest(quint32 id = 1)
{
    Request request;
    request.id = id;
    request.sourcePage = OwningPage;
    request.expectedPage = OwningPage;
    request.pageVisible = true;
    request.pageUrl =
        QUrl(QStringLiteral("https://portal.example/workouts"));
    request.downloadUrl =
        QUrl(QStringLiteral("https://cdn.example/activity.fit"));
    request.suggestedFileName = QStringLiteral("activity.fit");
    return request;
}

Completion completionFor(const Request &request,
                         const QString &path)
{
    Completion completion;
    completion.id = request.id;
    completion.sourcePage = request.sourcePage;
    completion.filePath = path;
    completion.finished = true;
    completion.succeeded = true;
    return completion;
}

bool writeFile(const QString &path,
               const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(contents) == contents.size()
        && file.flush();
}

Decision authorize(Gate &gate,
                   const Request &request,
                   const QTemporaryDir &staging)
{
    gate.handleRequest(
        request, QString(), UserDecision::NotAsked);
    return gate.handleRequest(
        request, staging.path(), UserDecision::Approved);
}

} // namespace

class TestWebDownloadImportPolicy : public QObject
{
    Q_OBJECT

private slots:
    void foreignPageIsIgnored();
    void missingPageIdentityIsCancelled_data();
    void missingPageIdentityIsCancelled();
    void hiddenOwningPageIsCancelled();
    void savePageDownloadIsCancelled();
    void declaredOversizedRequestIsCancelled();
    void unknownSizeCanBecomeKnownBeforeApproval();
    void sizeGrowingPastLimitBeforeApprovalIsCancelled();
    void unsafeUrlsAreCancelled_data();
    void unsafeUrlsAreCancelled();
    void loopbackHttpCanRequestConfirmation();
    void requestRequiresExplicitDecision();
    void rejectedRequestIsNotAuthorized();
    void approvalMustFollowTheExactPromptedRequest();
    void approvedRequestUsesPrivateStagingPath();
    void duplicateRequestCannotReplaceAuthorization();
    void concurrentRequestIsCancelledWhileDecisionPending();
    void concurrentRequestIsCancelledWhileDownloadPending();
    void completionRequiresAuthorization();
    void unfinishedCompletionDoesNotConsumeAuthorization();
    void failedCompletionConsumesAuthorization();
    void completionRequiresMatchingPage();
    void completionRequiresMatchingPath();
    void completedRegularFileIsAcceptedOnlyOnce();
    void missingFileIsRejected();
    void emptyFileIsRejected();
    void oversizedFileIsRejected();
#ifdef Q_OS_UNIX
    void symlinkFileIsRejected();
#endif
    void cancellationClearsAllRequestState();
    void safeFileNames_data();
    void safeFileNames();
};

void TestWebDownloadImportPolicy::foreignPageIsIgnored()
{
    Gate gate;
    Request request = validRequest();
    request.sourcePage = ForeignPage;

    const Decision decision = gate.handleRequest(
        request, QString(), UserDecision::NotAsked);

    QCOMPARE(decision.action, RequestAction::Ignore);
    QCOMPARE(gate.awaitingCount(), 0);
    QCOMPARE(gate.pendingCount(), 0);
}

void
TestWebDownloadImportPolicy::missingPageIdentityIsCancelled_data()
{
    QTest::addColumn<quintptr>("sourcePage");
    QTest::addColumn<quintptr>("expectedPage");

    QTest::newRow("missing-source")
        << quintptr(0) << OwningPage;
    QTest::newRow("missing-expected")
        << OwningPage << quintptr(0);
}

void TestWebDownloadImportPolicy::missingPageIdentityIsCancelled()
{
    QFETCH(quintptr, sourcePage);
    QFETCH(quintptr, expectedPage);
    Gate gate;
    Request request = validRequest();
    request.sourcePage = sourcePage;
    request.expectedPage = expectedPage;

    const Decision decision = gate.handleRequest(
        request, QString(), UserDecision::NotAsked);

    QCOMPARE(decision.action, RequestAction::Cancel);
    QCOMPARE(gate.awaitingCount(), 0);
    QCOMPARE(gate.pendingCount(), 0);
}

void TestWebDownloadImportPolicy::hiddenOwningPageIsCancelled()
{
    Gate gate;
    Request request = validRequest();
    request.pageVisible = false;

    const Decision decision = gate.handleRequest(
        request, QString(), UserDecision::NotAsked);

    QCOMPARE(decision.action, RequestAction::Cancel);
    QCOMPARE(gate.awaitingCount(), 0);
    QCOMPARE(gate.pendingCount(), 0);
}

void TestWebDownloadImportPolicy::savePageDownloadIsCancelled()
{
    Gate gate;
    Request request = validRequest();
    request.savePageDownload = true;

    const Decision decision = gate.handleRequest(
        request, QString(), UserDecision::NotAsked);

    QCOMPARE(decision.action, RequestAction::Cancel);
    QCOMPARE(gate.awaitingCount(), 0);
    QCOMPARE(gate.pendingCount(), 0);
}

void
TestWebDownloadImportPolicy::declaredOversizedRequestIsCancelled()
{
    Gate gate;
    Request request = validRequest();
    request.totalBytes = Gate::maximumImportBytes() + 1;

    const Decision decision = gate.handleRequest(
        request, QString(), UserDecision::NotAsked);

    QCOMPARE(decision.action, RequestAction::Cancel);
    QCOMPARE(gate.awaitingCount(), 0);
    QCOMPARE(gate.pendingCount(), 0);
}

void
TestWebDownloadImportPolicy::unknownSizeCanBecomeKnownBeforeApproval()
{
    Gate gate;
    const Request request = validRequest();
    QCOMPARE(gate.handleRequest(
                 request, QString(), UserDecision::NotAsked).action,
             RequestAction::Confirm);
    Request updated = request;
    updated.totalBytes = 1024;
    QTemporaryDir staging;
    QVERIFY(staging.isValid());

    const Decision decision = gate.handleRequest(
        updated, staging.path(), UserDecision::Approved);

    QCOMPARE(decision.action, RequestAction::Accept);
    QVERIFY(gate.hasPending(request.id));
}

void
TestWebDownloadImportPolicy::sizeGrowingPastLimitBeforeApprovalIsCancelled()
{
    Gate gate;
    const Request request = validRequest();
    QCOMPARE(gate.handleRequest(
                 request, QString(), UserDecision::NotAsked).action,
             RequestAction::Confirm);
    Request updated = request;
    updated.totalBytes = Gate::maximumImportBytes() + 1;
    QTemporaryDir staging;
    QVERIFY(staging.isValid());

    const Decision decision = gate.handleRequest(
        updated, staging.path(), UserDecision::Approved);

    QCOMPARE(decision.action, RequestAction::Cancel);
    QCOMPARE(gate.awaitingCount(), 0);
    QCOMPARE(gate.pendingCount(), 0);
}

void TestWebDownloadImportPolicy::unsafeUrlsAreCancelled_data()
{
    QTest::addColumn<QUrl>("pageUrl");
    QTest::addColumn<QUrl>("downloadUrl");

    const QUrl page(
        QStringLiteral("https://portal.example/workouts"));
    const QUrl download(
        QStringLiteral("https://cdn.example/activity.fit"));

    QTest::newRow("relative-page")
        << QUrl(QStringLiteral("/workouts")) << download;
    QTest::newRow("remote-http-page")
        << QUrl(QStringLiteral(
               "http://portal.example/workouts"))
        << download;
    QTest::newRow("remote-http-download")
        << page << QUrl(QStringLiteral(
               "http://cdn.example/activity.fit"));
    QTest::newRow("loopback-lookalike")
        << QUrl(QStringLiteral(
               "http://localhost.attacker.invalid/workouts"))
        << download;
    QTest::newRow("javascript-page")
        << QUrl(QStringLiteral("javascript:download()")) << download;
    QTest::newRow("page-userinfo")
        << QUrl(QStringLiteral(
               "https://user@portal.example/workouts"))
        << download;
    QTest::newRow("relative-download")
        << page << QUrl(QStringLiteral("activity.fit"));
    QTest::newRow("ftp-download")
        << page << QUrl(QStringLiteral(
               "ftp://cdn.example/activity.fit"));
    QTest::newRow("download-userinfo")
        << page << QUrl(QStringLiteral(
               "https://user@cdn.example/activity.fit"));
    QTest::newRow("remote-page-local-file")
        << page << QUrl::fromLocalFile(
               QStringLiteral("/tmp/activity.fit"));
    QTest::newRow("oversized-data-url")
        << page
        << QUrl(QStringLiteral("data:application/octet-stream,")
                + QString(9 * 1024, QLatin1Char('A')));
}

void TestWebDownloadImportPolicy::unsafeUrlsAreCancelled()
{
    QFETCH(QUrl, pageUrl);
    QFETCH(QUrl, downloadUrl);
    Gate gate;
    Request request = validRequest();
    request.pageUrl = pageUrl;
    request.downloadUrl = downloadUrl;

    const Decision decision = gate.handleRequest(
        request, QString(), UserDecision::NotAsked);

    QCOMPARE(decision.action, RequestAction::Cancel);
    QCOMPARE(gate.awaitingCount(), 0);
}

void
TestWebDownloadImportPolicy::loopbackHttpCanRequestConfirmation()
{
    Gate gate;
    Request request = validRequest();
    request.pageUrl =
        QUrl(QStringLiteral("http://127.0.0.1:8080/workouts"));
    request.downloadUrl =
        QUrl(QStringLiteral("http://localhost:9090/activity.fit"));

    const Decision decision = gate.handleRequest(
        request, QString(), UserDecision::NotAsked);

    QCOMPARE(decision.action, RequestAction::Confirm);
    QCOMPARE(gate.awaitingCount(), 1);
}

void TestWebDownloadImportPolicy::requestRequiresExplicitDecision()
{
    Gate gate;
    const Request request = validRequest();

    const Decision decision = gate.handleRequest(
        request, QString(), UserDecision::NotAsked);

    QCOMPARE(decision.action, RequestAction::Confirm);
    QCOMPARE(gate.awaitingCount(), 1);
    QCOMPARE(gate.pendingCount(), 0);
    QVERIFY(gate.takeCompletedImport(
        completionFor(request, QStringLiteral("/tmp/activity.fit")))
            .isEmpty());
    QCOMPARE(gate.pendingCount(), 0);
}

void TestWebDownloadImportPolicy::rejectedRequestIsNotAuthorized()
{
    Gate gate;
    const Request request = validRequest();
    QCOMPARE(gate.handleRequest(
                 request, QString(), UserDecision::NotAsked).action,
             RequestAction::Confirm);

    const Decision decision = gate.handleRequest(
        request, QString(), UserDecision::Rejected);

    QCOMPARE(decision.action, RequestAction::Cancel);
    QCOMPARE(gate.awaitingCount(), 0);
    QCOMPARE(gate.pendingCount(), 0);
}

void
TestWebDownloadImportPolicy::approvalMustFollowTheExactPromptedRequest()
{
    Gate gate;
    const Request request = validRequest();
    QCOMPARE(gate.handleRequest(
                 request, QString(), UserDecision::NotAsked).action,
             RequestAction::Confirm);
    Request changed = request;
    changed.downloadUrl =
        QUrl(QStringLiteral("https://attacker.invalid/activity.fit"));
    QTemporaryDir staging;
    QVERIFY(staging.isValid());

    const Decision decision = gate.handleRequest(
        changed, staging.path(), UserDecision::Approved);

    QCOMPARE(decision.action, RequestAction::Cancel);
    QCOMPARE(gate.awaitingCount(), 0);
    QCOMPARE(gate.pendingCount(), 0);
}

void
TestWebDownloadImportPolicy::approvedRequestUsesPrivateStagingPath()
{
    Gate gate;
    const Request request = validRequest();
    QTemporaryDir staging;
    QVERIFY(staging.isValid());

    const Decision decision = authorize(gate, request, staging);

    QCOMPARE(decision.action, RequestAction::Accept);
    QCOMPARE(decision.fileName, QStringLiteral("download.fit"));
    QCOMPARE(decision.filePath,
             staging.filePath(QStringLiteral("download.fit")));
    QCOMPARE(gate.awaitingCount(), 0);
    QCOMPARE(gate.pendingCount(), 1);
    QVERIFY(gate.hasPending(request.id));
}

void
TestWebDownloadImportPolicy::duplicateRequestCannotReplaceAuthorization()
{
    Gate gate;
    const Request request = validRequest();
    QTemporaryDir firstStaging;
    QTemporaryDir secondStaging;
    QVERIFY(firstStaging.isValid());
    QVERIFY(secondStaging.isValid());
    const Decision first = authorize(
        gate, request, firstStaging);
    QCOMPARE(first.action, RequestAction::Accept);

    const Decision duplicate = gate.handleRequest(
        request, QString(), UserDecision::NotAsked);
    const Decision replacement = gate.handleRequest(
        request, secondStaging.path(), UserDecision::Approved);

    QCOMPARE(duplicate.action, RequestAction::Cancel);
    QCOMPARE(replacement.action, RequestAction::Cancel);
    QCOMPARE(gate.pendingCount(), 1);
    QVERIFY(writeFile(first.filePath, QByteArrayLiteral("ride")));
    QCOMPARE(gate.takeCompletedImport(
                 completionFor(request, first.filePath)),
             first.filePath);
}

void
TestWebDownloadImportPolicy::concurrentRequestIsCancelledWhileDecisionPending()
{
    Gate gate;
    const Request first = validRequest(1);
    const Request second = validRequest(2);
    QCOMPARE(gate.handleRequest(
                 first, QString(), UserDecision::NotAsked).action,
             RequestAction::Confirm);

    const Decision decision = gate.handleRequest(
        second, QString(), UserDecision::NotAsked);

    QCOMPARE(decision.action, RequestAction::Cancel);
    QCOMPARE(gate.awaitingCount(), 1);
    QCOMPARE(gate.pendingCount(), 0);
}

void
TestWebDownloadImportPolicy::concurrentRequestIsCancelledWhileDownloadPending()
{
    Gate gate;
    const Request first = validRequest(1);
    const Request second = validRequest(2);
    QTemporaryDir staging;
    QVERIFY(staging.isValid());
    QCOMPARE(authorize(gate, first, staging).action,
             RequestAction::Accept);

    const Decision decision = gate.handleRequest(
        second, QString(), UserDecision::NotAsked);

    QCOMPARE(decision.action, RequestAction::Cancel);
    QCOMPARE(gate.awaitingCount(), 0);
    QCOMPARE(gate.pendingCount(), 1);
}

void TestWebDownloadImportPolicy::completionRequiresAuthorization()
{
    Gate gate;
    const Request request = validRequest();
    QTemporaryDir staging;
    QVERIFY(staging.isValid());
    const QString path =
        staging.filePath(QStringLiteral("download.fit"));
    QVERIFY(writeFile(path, QByteArrayLiteral("ride")));

    QVERIFY(gate.takeCompletedImport(
        completionFor(request, path)).isEmpty());
}

void
TestWebDownloadImportPolicy::unfinishedCompletionDoesNotConsumeAuthorization()
{
    Gate gate;
    const Request request = validRequest();
    QTemporaryDir staging;
    QVERIFY(staging.isValid());
    const Decision decision = authorize(gate, request, staging);
    QVERIFY(writeFile(decision.filePath, QByteArrayLiteral("ride")));
    Completion completion =
        completionFor(request, decision.filePath);
    completion.finished = false;

    QVERIFY(gate.takeCompletedImport(completion).isEmpty());
    QVERIFY(gate.hasPending(request.id));

    completion.finished = true;
    QCOMPARE(gate.takeCompletedImport(completion),
             decision.filePath);
    QVERIFY(!gate.hasPending(request.id));
}

void
TestWebDownloadImportPolicy::failedCompletionConsumesAuthorization()
{
    Gate gate;
    const Request request = validRequest();
    QTemporaryDir staging;
    QVERIFY(staging.isValid());
    const Decision decision = authorize(gate, request, staging);
    Completion completion =
        completionFor(request, decision.filePath);
    completion.succeeded = false;

    QVERIFY(gate.takeCompletedImport(completion).isEmpty());
    QVERIFY(!gate.hasPending(request.id));
}

void TestWebDownloadImportPolicy::completionRequiresMatchingPage()
{
    Gate gate;
    const Request request = validRequest();
    QTemporaryDir staging;
    QVERIFY(staging.isValid());
    const Decision decision = authorize(gate, request, staging);
    QVERIFY(writeFile(decision.filePath, QByteArrayLiteral("ride")));
    Completion completion =
        completionFor(request, decision.filePath);
    completion.sourcePage = ForeignPage;

    QVERIFY(gate.takeCompletedImport(completion).isEmpty());
    QVERIFY(!gate.hasPending(request.id));
}

void TestWebDownloadImportPolicy::completionRequiresMatchingPath()
{
    Gate gate;
    const Request request = validRequest();
    QTemporaryDir staging;
    QVERIFY(staging.isValid());
    const Decision decision = authorize(gate, request, staging);
    const QString other =
        staging.filePath(QStringLiteral("other.fit"));
    QVERIFY(writeFile(decision.filePath, QByteArrayLiteral("ride")));
    QVERIFY(writeFile(other, QByteArrayLiteral("other")));
    Completion completion = completionFor(request, other);

    QVERIFY(gate.takeCompletedImport(completion).isEmpty());
    QVERIFY(!gate.hasPending(request.id));
}

void
TestWebDownloadImportPolicy::completedRegularFileIsAcceptedOnlyOnce()
{
    Gate gate;
    const Request request = validRequest();
    QTemporaryDir staging;
    QVERIFY(staging.isValid());
    const Decision decision = authorize(gate, request, staging);
    QVERIFY(writeFile(decision.filePath, QByteArrayLiteral("ride")));
    const Completion completion =
        completionFor(request, decision.filePath);

    QCOMPARE(gate.takeCompletedImport(completion),
             decision.filePath);
    QVERIFY(gate.takeCompletedImport(completion).isEmpty());
    QVERIFY(!gate.hasPending(request.id));
}

void TestWebDownloadImportPolicy::missingFileIsRejected()
{
    Gate gate;
    const Request request = validRequest();
    QTemporaryDir staging;
    QVERIFY(staging.isValid());
    const Decision decision = authorize(gate, request, staging);

    QVERIFY(gate.takeCompletedImport(
        completionFor(request, decision.filePath)).isEmpty());
    QVERIFY(!gate.hasPending(request.id));
}

void TestWebDownloadImportPolicy::emptyFileIsRejected()
{
    Gate gate;
    const Request request = validRequest();
    QTemporaryDir staging;
    QVERIFY(staging.isValid());
    const Decision decision = authorize(gate, request, staging);
    QVERIFY(writeFile(decision.filePath, QByteArray()));

    QVERIFY(gate.takeCompletedImport(
        completionFor(request, decision.filePath)).isEmpty());
    QVERIFY(!gate.hasPending(request.id));
}

void TestWebDownloadImportPolicy::oversizedFileIsRejected()
{
    Gate gate;
    const Request request = validRequest();
    QTemporaryDir staging;
    QVERIFY(staging.isValid());
    const Decision decision = authorize(gate, request, staging);
    QFile file(decision.filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.resize(Gate::maximumImportBytes() + 1));
    file.close();

    QVERIFY(gate.takeCompletedImport(
        completionFor(request, decision.filePath)).isEmpty());
    QVERIFY(!gate.hasPending(request.id));
}

#ifdef Q_OS_UNIX
void TestWebDownloadImportPolicy::symlinkFileIsRejected()
{
    Gate gate;
    const Request request = validRequest();
    QTemporaryDir staging;
    QTemporaryDir outside;
    QVERIFY(staging.isValid());
    QVERIFY(outside.isValid());
    const Decision decision = authorize(gate, request, staging);
    const QString target =
        outside.filePath(QStringLiteral("target.fit"));
    QVERIFY(writeFile(target, QByteArrayLiteral("ride")));
    QVERIFY(QFile::link(target, decision.filePath));
    QVERIFY(QFileInfo(decision.filePath).isSymLink());

    QVERIFY(gate.takeCompletedImport(
        completionFor(request, decision.filePath)).isEmpty());
    QVERIFY(!gate.hasPending(request.id));
}
#endif

void TestWebDownloadImportPolicy::cancellationClearsAllRequestState()
{
    Gate gate;
    const Request awaiting = validRequest(1);
    const Request pending = validRequest(2);
    QTemporaryDir staging;
    QVERIFY(staging.isValid());
    QCOMPARE(gate.handleRequest(
                 awaiting, QString(), UserDecision::NotAsked).action,
             RequestAction::Confirm);

    gate.cancel(awaiting.id);
    QCOMPARE(gate.awaitingCount(), 0);
    QCOMPARE(gate.pendingCount(), 0);

    QCOMPARE(authorize(gate, pending, staging).action,
             RequestAction::Accept);
    gate.cancel(pending.id);
    QCOMPARE(gate.awaitingCount(), 0);
    QCOMPARE(gate.pendingCount(), 0);
}

void TestWebDownloadImportPolicy::safeFileNames_data()
{
    QTest::addColumn<QString>("suggested");
    QTest::addColumn<QString>("expected");

    QTest::newRow("fit")
        << QStringLiteral("activity.fit")
        << QStringLiteral("download.fit");
    QTest::newRow("uppercase")
        << QStringLiteral("ACTIVITY.TCX")
        << QStringLiteral("download.tcx");
    QTest::newRow("slash-traversal")
        << QStringLiteral("../../activity.fit")
        << QStringLiteral("download.fit");
    QTest::newRow("backslash-traversal")
        << QStringLiteral("..\\..\\activity.fit")
        << QStringLiteral("download.fit");
    QTest::newRow("compressed")
        << QStringLiteral("activity.fit.gz")
        << QStringLiteral("download.fit.gz");
    QTest::newRow("compressed-uppercase")
        << QStringLiteral("activity.FIT.ZIP")
        << QStringLiteral("download.fit.zip");
    QTest::newRow("no-extension")
        << QStringLiteral("activity")
        << QStringLiteral("download");
    QTest::newRow("hidden-file")
        << QStringLiteral(".bashrc")
        << QStringLiteral("download");
    QTest::newRow("separator-after-extension")
        << QStringLiteral("activity.fit/other")
        << QStringLiteral("download");
    QTest::newRow("punctuation")
        << QStringLiteral("activity.<fit>")
        << QStringLiteral("download");
    QTest::newRow("long-extension")
        << QStringLiteral("activity.abcdefghijklmnopq")
        << QStringLiteral("download");
    QTest::newRow("non-ascii-extension")
        << (QStringLiteral("activity.f")
            + QChar(0x00ef)
            + QStringLiteral("t"))
        << QStringLiteral("download");
}

void TestWebDownloadImportPolicy::safeFileNames()
{
    QFETCH(QString, suggested);
    QFETCH(QString, expected);

    QCOMPARE(Gate::safeFileName(suggested), expected);
}

QTEST_MAIN(TestWebDownloadImportPolicy)
#include "testWebDownloadImportPolicy.moc"
