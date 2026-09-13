#include "WorkoutGame3DHealth.h"

#include <QtTest>

#include <limits>

class TestWorkoutGame3DHealth : public QObject
{
    Q_OBJECT

private slots:
    void acceptsHealthyScene();
    void reportsInvalidAndUndergroundCamera();
    void reportsRiderOutsideFrustum();
    void rateLimitsHealthyAndRepeatedAnomalyLogs();
    void logsChangedAnomalyImmediately();
};

namespace {

WorkoutGame3DHealthInput healthyInput()
{
    WorkoutGame3DHealthInput input;
    input.worldReady = true;
    input.camera = {0.0, 4.0, -8.0};
    input.cameraTarget = {0.0, 1.0, 4.0};
    input.rider = {0.0, 1.0, 0.0};
    input.cameraGroundY = 0.0;
    input.riderGroundY = 0.0;
    input.verticalFieldOfViewDegrees = 47.0;
    input.aspectRatio = 16.0 / 9.0;
    input.nearClipMeters = 0.15;
    input.farClipMeters = 650.0;
    input.visibleTriangles = 12000;
    return input;
}

}

void TestWorkoutGame3DHealth::acceptsHealthyScene()
{
    const WorkoutGame3DHealthSnapshot snapshot =
            WorkoutGame3DHealth::evaluate(healthyInput());

    QVERIFY(snapshot.healthy());
    QCOMPARE(snapshot.reasonCodes(), QStringLiteral("ok"));
    QVERIFY(snapshot.riderInFrustum);
    QVERIFY(snapshot.riderDepthMeters > 0.0);
    QCOMPARE(snapshot.cameraGroundClearanceMeters, 4.0);
    QCOMPARE(snapshot.verticalFieldOfViewDegrees, 47.0);
    QCOMPARE(snapshot.aspectRatio, 16.0 / 9.0);
}

void TestWorkoutGame3DHealth::reportsInvalidAndUndergroundCamera()
{
    WorkoutGame3DHealthInput input = healthyInput();
    input.camera.y = -0.2;
    WorkoutGame3DHealthSnapshot snapshot =
            WorkoutGame3DHealth::evaluate(input);
    QVERIFY(snapshot.reasons.testFlag(
            WorkoutGame3DHealthReason::CameraBelowGround));

    input.camera.x = std::numeric_limits<double>::quiet_NaN();
    snapshot = WorkoutGame3DHealth::evaluate(input);
    QVERIFY(snapshot.reasons.testFlag(
            WorkoutGame3DHealthReason::NonFinitePose));
}

void TestWorkoutGame3DHealth::reportsRiderOutsideFrustum()
{
    WorkoutGame3DHealthInput input = healthyInput();
    input.rider = {30.0, 1.0, 0.0};
    WorkoutGame3DHealthSnapshot snapshot =
            WorkoutGame3DHealth::evaluate(input);
    QVERIFY(snapshot.reasons.testFlag(
            WorkoutGame3DHealthReason::RiderOutsideFrustum));

    input = healthyInput();
    input.rider.z = -20.0;
    snapshot = WorkoutGame3DHealth::evaluate(input);
    QVERIFY(snapshot.reasons.testFlag(
            WorkoutGame3DHealthReason::RiderBehindCamera));
}

void TestWorkoutGame3DHealth::rateLimitsHealthyAndRepeatedAnomalyLogs()
{
    WorkoutGame3DHealthMonitor monitor;
    const WorkoutGame3DHealthSnapshot healthy =
            WorkoutGame3DHealth::evaluate(healthyInput());
    QVERIFY(monitor.shouldLog(healthy, 1000));
    QVERIFY(!monitor.shouldLog(healthy, 10999));
    QVERIFY(monitor.shouldLog(healthy, 11000));

    WorkoutGame3DHealthInput input = healthyInput();
    input.camera.y = -1.0;
    const WorkoutGame3DHealthSnapshot anomaly =
            WorkoutGame3DHealth::evaluate(input);
    QVERIFY(monitor.shouldLog(anomaly, 12000));
    QVERIFY(!monitor.shouldLog(anomaly, 16999));
    QVERIFY(monitor.shouldLog(anomaly, 17000));
}

void TestWorkoutGame3DHealth::logsChangedAnomalyImmediately()
{
    WorkoutGame3DHealthMonitor monitor;
    WorkoutGame3DHealthInput input = healthyInput();
    input.camera.y = -1.0;
    const WorkoutGame3DHealthSnapshot underground =
            WorkoutGame3DHealth::evaluate(input);
    QVERIFY(monitor.shouldLog(underground, 1000));

    input = healthyInput();
    input.rider.x = 30.0;
    const WorkoutGame3DHealthSnapshot outOfView =
            WorkoutGame3DHealth::evaluate(input);
    QVERIFY(!monitor.shouldLog(outOfView, 1500));
    QVERIFY(monitor.shouldLog(outOfView, 2000));

    const WorkoutGame3DHealthSnapshot recovered =
            WorkoutGame3DHealth::evaluate(healthyInput());
    QVERIFY(monitor.shouldLog(recovered, 3000));
}

QTEST_APPLESS_MAIN(TestWorkoutGame3DHealth)

#include "testWorkoutGame3DHealth.moc"
