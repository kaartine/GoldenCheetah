#include <QtTest>

#include "MergeActivityAlignment.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <utility>

namespace {

QVector<double> deterministicSignal(int count, quint32 seed = 0x4d595df4U)
{
    QVector<double> values;
    values.reserve(count);

    quint32 state = seed;
    for (int index = 0; index < count; ++index) {
        state = state * 1664525U + 1013904223U;
        const double noise = double((state >> 8) & 0xffffU) / 65535.0;
        values.append(120.0
                      + 18.0 * std::sin(double(index) * 0.017)
                      + 7.0 * std::cos(double(index) * 0.071)
                      + noise);
    }
    return values;
}

MergeActivityAlignment::Result legacyBestOffset(
    const QVector<double> &base,
    const QVector<double> &fit)
{
    MergeActivityAlignment::Result result;

    for (int offset = -(base.size() / 3);
         offset < base.size() / 3;
         ++offset) {
        double mean = 0.0;
        int meanCount = 0;
        for (int index = 0; index + offset < base.size(); ++index) {
            if (index + offset > 0) {
                mean += base.at(index + offset);
            }
            ++meanCount;
        }
        mean /= double(meanCount);

        double residual = 0.0;
        double total = 0.0;
        for (int index = 0;
             index + offset < base.size() && index < fit.size();
             ++index) {
            if (index + offset > 0) {
                const double baseValue = base.at(index + offset);
                const double delta = baseValue - fit.at(index);
                residual += delta * delta;
                const double centered = baseValue - mean;
                total += centered * centered;
            }
        }

        const double rSquared = 1.0 - residual / total;
        if (rSquared > result.rSquared) {
            result.valid = true;
            result.offset = offset;
            result.rSquared = rSquared;
        }
    }
    return result;
}

MergeActivityAlignment::Series shiftedSeries(
    int key,
    int sampleCount,
    int offset,
    quint32 seed = 0x4d595df4U)
{
    const int leading = std::max(0, -offset);
    const QVector<double> timeline =
        deterministicSignal(sampleCount + std::abs(offset) + 300, seed);

    MergeActivityAlignment::Series series;
    series.key = key;
    series.base = timeline.mid(leading, sampleCount);
    series.fit = timeline.mid(leading + offset, sampleCount - 300);
    return series;
}

qint64 slowestAlignmentMs(
    const QVector<double> &base,
    const QVector<double> &fit,
    int repetitions)
{
    qint64 slowest = 0;
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        QElapsedTimer timer;
        timer.start();
        const MergeActivityAlignment::Result result =
            MergeActivityAlignment::findBestOffset(base, fit);
        slowest = std::max(slowest, timer.elapsed());
        if (!result.valid || result.cancelled || result.offset != 120) {
            return -1;
        }
    }
    return slowest;
}

} // namespace

class TestMergeActivityAlignment : public QObject
{
    Q_OBJECT

private slots:
    void findsPositiveOffset();
    void findsNegativeOffset();
    void matchesLegacyAcrossDeterministicFixtures();
    void matchesLegacyOnFftPath();
    void matchesLegacyAtDirectFftBoundary();
    void preservesTieOrderBeyondExactCandidateLimit();
    void preservesLegacyConstantSeriesBehavior();
    void allZeroSeriesHasNoWinner();
    void batchPreservesSeriesTieOrder();
    void cancellationIsCooperative();
    void runnerKeepsEventLoopResponsive();
    void runnerCanBeCancelled();
    void runnerDestructionCancelsAndJoins();
    void alignsOneAndThreeHourSeriesWithinUiBudget();
};

void TestMergeActivityAlignment::findsPositiveOffset()
{
    const MergeActivityAlignment::Series series =
        shiftedSeries(1, 512, 73);

    const MergeActivityAlignment::Result result =
        MergeActivityAlignment::findBestOffset(series.base, series.fit);

    QVERIFY(result.valid);
    QVERIFY(!result.cancelled);
    QCOMPARE(result.offset, 73);
    QVERIFY(std::abs(result.rSquared - 1.0) < 1.0e-9);
}

void TestMergeActivityAlignment::findsNegativeOffset()
{
    const MergeActivityAlignment::Series series =
        shiftedSeries(1, 512, -47);

    const MergeActivityAlignment::Result result =
        MergeActivityAlignment::findBestOffset(series.base, series.fit);

    QVERIFY(result.valid);
    QVERIFY(!result.cancelled);
    QCOMPARE(result.offset, -47);
    QVERIFY(std::abs(result.rSquared - 1.0) < 1.0e-9);
}

void TestMergeActivityAlignment::matchesLegacyAcrossDeterministicFixtures()
{
    for (int fixture = 0; fixture < 24; ++fixture) {
        const int baseCount = 64 + fixture * 7;
        const int fitCount = 31 + (fixture * 11) % (baseCount - 31);
        const QVector<double> base =
            deterministicSignal(baseCount, 0x10203040U + fixture);
        QVector<double> fit =
            deterministicSignal(fitCount, 0x10203040U + fixture * 3);
        for (int index = 0; index < fit.size(); ++index) {
            fit[index] += 0.1 * std::sin(double(index + fixture) * 0.13);
        }

        const MergeActivityAlignment::Result expected =
            legacyBestOffset(base, fit);
        const MergeActivityAlignment::Result actual =
            MergeActivityAlignment::findBestOffset(base, fit);

        QCOMPARE(actual.cancelled, false);
        QCOMPARE(actual.valid, expected.valid);
        QCOMPARE(actual.offset, expected.offset);
        QVERIFY2(std::abs(actual.rSquared - expected.rSquared) < 1.0e-8,
                 qPrintable(QStringLiteral("fixture %1 score %2 != %3")
                                .arg(fixture)
                                .arg(actual.rSquared, 0, 'g', 17)
                                .arg(expected.rSquared, 0, 'g', 17)));
    }
}

void TestMergeActivityAlignment::matchesLegacyOnFftPath()
{
    const QVector<int> offsets = {-127, -41, 0, 57, 121, 189};
    for (int fixture = 0; fixture < offsets.size(); ++fixture) {
        MergeActivityAlignment::Series series =
            shiftedSeries(
                fixture,
                640 + fixture * 79,
                offsets.at(fixture),
                0x20406080U + fixture);
        for (int index = 0; index < series.fit.size(); ++index) {
            series.fit[index] +=
                0.02 * std::sin(double(index + fixture) * 0.11);
        }

        const MergeActivityAlignment::Result expected =
            legacyBestOffset(series.base, series.fit);
        const MergeActivityAlignment::Result actual =
            MergeActivityAlignment::findBestOffset(series.base, series.fit);

        QVERIFY(expected.valid);
        QCOMPARE(actual.cancelled, false);
        QCOMPARE(actual.valid, expected.valid);
        QCOMPARE(actual.offset, expected.offset);
        QVERIFY2(std::abs(actual.rSquared - expected.rSquared) < 1.0e-8,
                 qPrintable(QStringLiteral("fixture %1 score %2 != %3")
                                .arg(fixture)
                                .arg(actual.rSquared, 0, 'g', 17)
                                .arg(expected.rSquared, 0, 'g', 17)));
    }
}

void TestMergeActivityAlignment::matchesLegacyAtDirectFftBoundary()
{
    for (int sampleCount : {512, 513}) {
        MergeActivityAlignment::Series series =
            shiftedSeries(1, sampleCount, 73, 0x30405060U);
        for (int index = 0; index < series.fit.size(); ++index) {
            series.fit[index] += 0.01 * std::cos(double(index) * 0.09);
        }

        const MergeActivityAlignment::Result expected =
            legacyBestOffset(series.base, series.fit);
        const MergeActivityAlignment::Result actual =
            MergeActivityAlignment::findBestOffset(series.base, series.fit);

        QVERIFY(expected.valid);
        QCOMPARE(actual.valid, expected.valid);
        QCOMPARE(actual.cancelled, false);
        QCOMPARE(actual.offset, expected.offset);
        QVERIFY(std::abs(actual.rSquared - expected.rSquared) < 1.0e-8);
    }
}

void TestMergeActivityAlignment::preservesTieOrderBeyondExactCandidateLimit()
{
    QVector<double> base(1200);
    QVector<double> fit(900);
    const QVector<double> pattern = {100.0, 120.0, 140.0, 120.0};
    for (int index = 0; index < base.size(); ++index) {
        base[index] = pattern.at(index % pattern.size());
    }
    for (int index = 0; index < fit.size(); ++index) {
        fit[index] = pattern.at(index % pattern.size());
    }

    int perfectCandidates = 0;
    const int searchRadius = base.size() / 3;
    for (int offset = -searchRadius; offset < searchRadius; ++offset) {
        const int lower = std::max(0, 1 - offset);
        const int upper = std::min(fit.size(), base.size() - offset);
        bool exactMatch = lower < upper;
        for (int index = lower; exactMatch && index < upper; ++index) {
            exactMatch = base.at(index + offset) == fit.at(index);
        }
        if (exactMatch) ++perfectCandidates;
    }
    QVERIFY(perfectCandidates > 64);

    const MergeActivityAlignment::Result expected =
        legacyBestOffset(base, fit);
    const MergeActivityAlignment::Result actual =
        MergeActivityAlignment::findBestOffset(base, fit);

    QVERIFY(expected.valid);
    QCOMPARE(actual.valid, expected.valid);
    QCOMPARE(actual.cancelled, false);
    QCOMPARE(actual.offset, expected.offset);
    QVERIFY(std::abs(actual.rSquared - expected.rSquared) < 1.0e-8);
}

void TestMergeActivityAlignment::preservesLegacyConstantSeriesBehavior()
{
    const QVector<double> base(1024, 140.0);
    const QVector<double> fit(700, 140.0);
    const MergeActivityAlignment::Result expected =
        legacyBestOffset(base, fit);

    const MergeActivityAlignment::Result actual =
        MergeActivityAlignment::findBestOffset(base, fit);

    QVERIFY(expected.valid);
    QCOMPARE(actual.valid, expected.valid);
    QCOMPARE(actual.offset, expected.offset);
    QCOMPARE(actual.rSquared, expected.rSquared);
}

void TestMergeActivityAlignment::allZeroSeriesHasNoWinner()
{
    const QVector<double> base(1024, 0.0);
    const QVector<double> fit(700, 0.0);

    const MergeActivityAlignment::Result result =
        MergeActivityAlignment::findBestOffset(base, fit);

    QVERIFY(!result.valid);
    QVERIFY(!result.cancelled);
    QCOMPARE(result.offset, 0);
    QCOMPARE(result.rSquared, 0.0);
}

void TestMergeActivityAlignment::batchPreservesSeriesTieOrder()
{
    const MergeActivityAlignment::Series first =
        shiftedSeries(7, 512, 31, 0x11223344U);
    MergeActivityAlignment::Series second = first;
    second.key = 9;

    const MergeActivityAlignment::BatchResult result =
        MergeActivityAlignment::findBestSeries({first, second});

    QVERIFY(result.valid);
    QVERIFY(!result.cancelled);
    QCOMPARE(result.seriesKey, 7);
    QCOMPARE(result.offset, 31);
}

void TestMergeActivityAlignment::cancellationIsCooperative()
{
    const MergeActivityAlignment::Series series =
        shiftedSeries(1, 10800, 120);
    std::atomic_bool cancelled(true);

    const MergeActivityAlignment::Result result =
        MergeActivityAlignment::findBestOffset(
            series.base, series.fit, &cancelled);

    QVERIFY(result.cancelled);
    QVERIFY(!result.valid);
}

void TestMergeActivityAlignment::runnerKeepsEventLoopResponsive()
{
    QVector<MergeActivityAlignment::Series> inputs;
    for (int key = 0; key < 12; ++key) {
        inputs.append(shiftedSeries(key, 10800, 120, 0x70000000U + key));
    }

    MergeActivityAlignment::Runner runner;
    MergeActivityAlignment::BatchResult completedResult;
    bool completed = false;
    int heartbeats = 0;
    QEventLoop loop;
    QTimer heartbeat;
    heartbeat.setInterval(0);
    connect(&heartbeat, &QTimer::timeout, this, [&heartbeats]() {
        ++heartbeats;
    });
    connect(&runner, &MergeActivityAlignment::Runner::completed,
            this,
            [&completed, &completedResult, &loop](
                const MergeActivityAlignment::BatchResult &result) {
                completed = true;
                completedResult = result;
                loop.quit();
            });

    QElapsedTimer startTimer;
    startTimer.start();
    QVERIFY(runner.start(std::move(inputs)));
    QVERIFY2(startTimer.elapsed() < 50,
             "starting alignment blocked the caller");

    heartbeat.start();
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    heartbeat.stop();

    QVERIFY2(completed, "asynchronous alignment timed out");
    QVERIFY(completedResult.valid);
    QVERIFY(!completedResult.cancelled);
    QCOMPARE(completedResult.offset, 120);
    QVERIFY2(heartbeats > 0, "event loop did not run during alignment");
}

void TestMergeActivityAlignment::runnerCanBeCancelled()
{
    QVector<MergeActivityAlignment::Series> inputs;
    for (int key = 0; key < 24; ++key) {
        inputs.append(shiftedSeries(key, 10800, 120, 0x50000000U + key));
    }

    MergeActivityAlignment::Runner runner;
    MergeActivityAlignment::BatchResult completedResult;
    bool completed = false;
    QEventLoop loop;
    connect(&runner, &MergeActivityAlignment::Runner::completed,
            this,
            [&completed, &completedResult, &loop](
                const MergeActivityAlignment::BatchResult &result) {
                completed = true;
                completedResult = result;
                loop.quit();
            });

    QVERIFY(runner.start(std::move(inputs)));
    runner.cancel();
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();

    QVERIFY2(completed, "cancelled alignment did not terminate");
    QVERIFY(completedResult.cancelled);
    QVERIFY(!completedResult.valid);
}

void TestMergeActivityAlignment::runnerDestructionCancelsAndJoins()
{
    QVector<MergeActivityAlignment::Series> inputs;
    for (int key = 0; key < 64; ++key) {
        inputs.append(shiftedSeries(key, 10800, 120, 0x60000000U + key));
    }

    int completions = 0;
    QElapsedTimer timer;
    timer.start();
    auto runner = std::make_unique<MergeActivityAlignment::Runner>();
    connect(runner.get(), &MergeActivityAlignment::Runner::completed,
            this, [&completions]() { ++completions; });
    QVERIFY(runner->start(std::move(inputs)));
    runner.reset();

    QVERIFY2(timer.elapsed() < 5000,
             "destroying an active runner did not cancel and join promptly");
    QCoreApplication::processEvents();
    QCOMPARE(completions, 0);
}

void TestMergeActivityAlignment::alignsOneAndThreeHourSeriesWithinUiBudget()
{
    const MergeActivityAlignment::Series oneHour =
        shiftedSeries(1, 3600, 120);
    const MergeActivityAlignment::Series threeHour =
        shiftedSeries(1, 10800, 120);

    const qint64 oneHourMs =
        slowestAlignmentMs(oneHour.base, oneHour.fit, 3);
    const qint64 threeHourMs =
        slowestAlignmentMs(threeHour.base, threeHour.fit, 3);

    QVERIFY2(oneHourMs >= 0, "one-hour fixture produced the wrong offset");
    QVERIFY2(threeHourMs >= 0, "three-hour fixture produced the wrong offset");

#if defined(__SANITIZE_ADDRESS__)
    const qint64 uiBudgetMs = 1500;
#else
    const qint64 uiBudgetMs = 350;
#endif
    QVERIFY2(threeHourMs < uiBudgetMs,
             qPrintable(QStringLiteral("three-hour alignment took %1 ms")
                            .arg(threeHourMs)));
    QVERIFY2(threeHourMs <= oneHourMs * 7 + 50,
             qPrintable(QStringLiteral("scaling was %1 ms -> %2 ms")
                            .arg(oneHourMs)
                            .arg(threeHourMs)));
}

QTEST_GUILESS_MAIN(TestMergeActivityAlignment)
#include "testMergeActivityAlignment.moc"
