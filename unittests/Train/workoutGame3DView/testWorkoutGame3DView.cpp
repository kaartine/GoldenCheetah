/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGame3DViewModel.h"
#include "WorkoutGameBermGeometry.h"
#include "WorkoutGameClimbGeometry.h"
#include "WorkoutGame3DTerrainProfile.h"
#include "WorkoutGame3DWindow.h"
#include "WorkoutGameFeatureGeometry.h"
#include "WorkoutGameRootGeometry.h"
#include "WorkoutGameRockGardenGeometry.h"
#include "WorkoutGameRockSlabGeometry.h"
#include "WorkoutGameSkinnyGeometry.h"
#include "WorkoutGameTrailBranch.h"
#include "Train/TrainerTargetCoordinator.h"
#include "Train/TrainingRecordingIo.h"
#include "Train/WorkoutGameFeatureRuntime.h"
#include "Train/WorkoutGameWorld.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QImageWriter>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickView>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QSet>
#include <QSignalSpy>
#include <QSGRendererInterface>
#include <QTemporaryFile>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace {

constexpr double FtpWatts = 200.0;

class PerformanceTargetDevice final : public TrainerTargetDevice
{
public:
    void setLoad(double) override { ++loadCalls; }
    void setGradient(double) override { ++gradientCalls; }
    void setWindResistance(double) override { ++windCalls; }

    int callCount() const { return loadCalls + gradientCalls + windCalls; }

private:
    int loadCalls = 0;
    int gradientCalls = 0;
    int windCalls = 0;
};

struct PeriodicDeadlineProbe
{
    explicit PeriodicDeadlineProbe(qint64 interval) :
        intervalMs(interval),
        nextDeadlineMs(interval)
    {
    }

    void record(qint64 elapsedMs)
    {
        const qint64 lateness = std::max<qint64>(
                0, elapsedMs - nextDeadlineMs);
        samples.push_back(double(lateness));
        const qint64 skipped = lateness / intervalMs;
        missedDeadlines += int(skipped);
        nextDeadlineMs += (skipped + 1) * intervalMs;
    }

    double percentile(double fraction) const
    {
        if (samples.empty()) return 0.0;
        QVector<double> sorted = samples;
        std::sort(sorted.begin(), sorted.end());
        const qsizetype rank = std::clamp<qsizetype>(
                qsizetype(std::ceil(fraction * double(sorted.size()))) - 1,
                0, sorted.size() - 1);
        return sorted[rank];
    }

    double maximum() const
    {
        return samples.empty()
                ? 0.0 : *std::max_element(samples.begin(), samples.end());
    }

    qint64 intervalMs = 0;
    qint64 nextDeadlineMs = 0;
    QVector<double> samples;
    int missedDeadlines = 0;
};

struct ServiceLatencyPhase
{
    PeriodicDeadlineProbe telemetry{20};
    PeriodicDeadlineProbe trainer{100};
    PeriodicDeadlineProbe recording{250};
    bool recordingOk = true;
    int trainerCalls = 0;
};

class ScopedEnvironmentVariable
{
public:
    explicit ScopedEnvironmentVariable(const char *variable) :
        name(variable),
        value(qgetenv(variable)),
        present(qEnvironmentVariableIsSet(variable))
    {
    }

    ~ScopedEnvironmentVariable()
    {
        if (present) {
            qputenv(name.constData(), value);
        } else {
            qunsetenv(name.constData());
        }
    }

private:
    QByteArray name;
    QByteArray value;
    bool present;
};

struct FeatureCatalogEntry
{
    WorkoutGameTerrainKind terrain;
    const char *name;
};

constexpr std::array<FeatureCatalogEntry, 11> FeatureCatalog = {{
    {WorkoutGameTerrainKind::Roots, "roots"},
    {WorkoutGameTerrainKind::Rollers, "rollers"},
    {WorkoutGameTerrainKind::Climb, "climb"},
    {WorkoutGameTerrainKind::RockGarden, "rock-garden"},
    {WorkoutGameTerrainKind::BunnyHop, "bunny-hop"},
    {WorkoutGameTerrainKind::Drop, "drop"},
    {WorkoutGameTerrainKind::Skinny, "skinny"},
    {WorkoutGameTerrainKind::Berm, "berm"},
    {WorkoutGameTerrainKind::LogOver, "log-over"},
    {WorkoutGameTerrainKind::Tabletop, "tabletop"},
    {WorkoutGameTerrainKind::RockSlab, "rock-slab"}
}};

WorkoutGameCourse catalogCourse(WorkoutGameTerrainKind terrain)
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 9173u + std::uint32_t(terrain);
    course.durationMs = 30000;
    WorkoutGameSection section;
    section.feature = terrain == WorkoutGameTerrainKind::Climb
            ? WorkoutGameFeature::Climb
            : terrain == WorkoutGameTerrainKind::BunnyHop
                    || terrain == WorkoutGameTerrainKind::LogOver
                    || terrain == WorkoutGameTerrainKind::Tabletop
            ? WorkoutGameFeature::SprintJump
            : terrain == WorkoutGameTerrainKind::Drop
            ? WorkoutGameFeature::RecoveryDescent
            : WorkoutGameFeature::Trail;
    section.terrain = terrain;
    section.durationMs = course.durationMs;
    section.targetWatts = 220.0;
    section.gradePercent = terrain == WorkoutGameTerrainKind::Climb
            ? 8.0 : terrain == WorkoutGameTerrainKind::Drop ? -6.0 : 0.0;
    section.difficulty = 0.65;
    section.challengeCount = 1;
    course.sections.push_back(section);
    return course;
}

WorkoutGameCourse climbRenderCourse()
{
    WorkoutGameCourse course = catalogCourse(WorkoutGameTerrainKind::Climb);
    WorkoutGameSection &climb = course.sections.front();
    climb.lengthMeters = 90.0;
    WorkoutGameSection runout;
    runout.feature = WorkoutGameFeature::RecoveryDescent;
    runout.terrain = WorkoutGameTerrainKind::SmoothTrail;
    runout.startMs = climb.durationMs;
    runout.durationMs = 12000;
    runout.lengthMeters = 36.0;
    runout.targetWatts = 100.0;
    runout.gradePercent = -3.0;
    course.sections.push_back(runout);
    course.durationMs += runout.durationMs;
    return course;
}

WorkoutGameCourse sampleCourse()
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 0x334456u;
    const WorkoutGameTerrainKind terrains[] = {
        WorkoutGameTerrainKind::Climb,
        WorkoutGameTerrainKind::Roots,
        WorkoutGameTerrainKind::Tabletop,
        WorkoutGameTerrainKind::RockGarden,
        WorkoutGameTerrainKind::Drop
    };
    std::int64_t startMs = 0;
    for (int index = 0; index < 5; ++index) {
        WorkoutGameSection section;
        section.feature = index == 2
                ? WorkoutGameFeature::SprintJump
                : WorkoutGameFeature::Trail;
        section.terrain = terrains[index];
        section.startMs = startMs;
        section.durationMs = 20000;
        section.targetWatts = 175.0 + index * 22.0;
        section.gradePercent = index == 0 ? 8.0 : (index == 4 ? -7.0 : 2.0);
        section.difficulty = 0.35 + index * 0.1;
        section.challengeCount = 1;
        section.visualVariant = std::uint32_t(index + 1);
        section.gravityAssisted = index == 4;
        course.sections.push_back(section);
        startMs += section.durationMs;
    }
    course.durationMs = startMs;
    return course;
}

WorkoutGameCourse cameraMotionCourse()
{
    WorkoutGameCourse course = sampleCourse();
    const std::array<double, 5> lengths = {{18.0, 16.0, 28.0, 16.0, 24.0}};
    for (std::size_t index = 0; index < course.sections.size(); ++index) {
        course.sections[index].lengthMeters = lengths[index];
    }
    return course;
}

WorkoutGameCourse renderBudgetCourse()
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 0x51a7u;
    std::int64_t startMs = 0;
    const std::array<WorkoutGameTerrainKind, 11> terrains = {{
        WorkoutGameTerrainKind::BunnyHop,
        WorkoutGameTerrainKind::LogOver,
        WorkoutGameTerrainKind::Drop,
        WorkoutGameTerrainKind::Roots,
        WorkoutGameTerrainKind::RockGarden,
        WorkoutGameTerrainKind::Skinny,
        WorkoutGameTerrainKind::Climb,
        WorkoutGameTerrainKind::Berm,
        WorkoutGameTerrainKind::Tabletop,
        WorkoutGameTerrainKind::RockSlab,
        WorkoutGameTerrainKind::Rollers
    }};
    for (WorkoutGameTerrainKind terrain : terrains) {
        WorkoutGameSection section = catalogCourse(terrain).sections.front();
        section.startMs = startMs;
        section.durationMs = 30000;
        section.lengthMeters = 42.0;
        course.sections.push_back(section);
        startMs += section.durationMs;
    }
    course.durationMs = startMs;
    return course;
}

WorkoutGameVisualSnapshot frameAt(
        const WorkoutGameRoadCourse &road,
        double distanceMeters)
{
    const WorkoutGameRoadSample roadSample =
            WorkoutGameRoadCourseBuilder::sample(road, distanceMeters);
    WorkoutGameVisualSnapshot frame;
    frame.world.ready = true;
    frame.world.terrain = roadSample.terrain;
    frame.world.gradePercent = roadSample.center.gradePercent;
    frame.world.rider.distanceMeters = distanceMeters;
    frame.world.rider.elevationMeters = roadSample.center.elevationMeters;
    frame.world.rider.pitchDegrees = roadSample.center.gradePercent * 0.45;
    frame.simulation.ready = true;
    frame.simulation.workoutTimeMs = 75400;
    frame.simulation.speedKph = 22.5;
    frame.riderPedalCycles = distanceMeters * 0.35;
    return frame;
}

ServiceLatencyPhase measureServiceLatency(
        int durationMs,
        const std::function<void(qint64)> &framePump = {})
{
    ServiceLatencyPhase phase;
    PerformanceTargetDevice targetDevice;
    TrainerTargetCoordinator targetCoordinator;
    QTemporaryFile recording;
    phase.recordingOk = recording.open();
    if (!phase.recordingOk) return phase;

    QElapsedTimer clock;
    QTimer telemetryTimer;
    QTimer trainerTimer;
    QTimer recordingTimer;
    QTimer frameTimer;
    QTimer phaseTimer;
    QEventLoop phaseLoop;
    telemetryTimer.setTimerType(Qt::PreciseTimer);
    trainerTimer.setTimerType(Qt::PreciseTimer);
    recordingTimer.setTimerType(Qt::PreciseTimer);
    frameTimer.setTimerType(Qt::PreciseTimer);
    phaseTimer.setTimerType(Qt::PreciseTimer);
    phaseTimer.setSingleShot(true);
    telemetryTimer.setInterval(int(phase.telemetry.intervalMs));
    trainerTimer.setInterval(int(phase.trainer.intervalMs));
    recordingTimer.setInterval(int(phase.recording.intervalMs));
    frameTimer.setInterval(20);
    QObject::connect(&telemetryTimer, &QTimer::timeout, &telemetryTimer, [&]() {
        phase.telemetry.record(clock.elapsed());
    });
    QObject::connect(&trainerTimer, &QTimer::timeout, &trainerTimer, [&]() {
        const qint64 elapsedMs = clock.elapsed();
        phase.trainer.record(elapsedMs);
        const TrainerTarget target = TrainerTarget::erg(220.0, elapsedMs);
        phase.recordingOk = phase.recordingOk
                && targetCoordinator.apply(
                    target,
                    std::vector<TrainerTargetDevice *>{&targetDevice})
                    == TrainerTargetResult::Applied;
    });
    QObject::connect(
            &recordingTimer, &QTimer::timeout, &recordingTimer, [&]() {
        const qint64 elapsedMs = clock.elapsed();
        phase.recording.record(elapsedMs);
        const QByteArray row = QByteArray::number(elapsedMs)
                + QByteArrayLiteral(",88,150,220\n");
        phase.recordingOk = phase.recordingOk
                && TrainingRecordingIo::writeAndFlush(
                    recording, row, [&recording]() {
                        return recording.flush();
                    }).ok();
    });
    if (framePump) {
        QObject::connect(&frameTimer, &QTimer::timeout, &frameTimer, [&]() {
            framePump(clock.elapsed());
        });
    }
    QObject::connect(
            &phaseTimer, &QTimer::timeout, &phaseLoop, &QEventLoop::quit);

    clock.start();
    telemetryTimer.start();
    trainerTimer.start();
    recordingTimer.start();
    if (framePump) frameTimer.start();
    phaseTimer.start(durationMs);
    phaseLoop.exec();
    frameTimer.stop();
    recordingTimer.stop();
    trainerTimer.stop();
    telemetryTimer.stop();
    phase.trainerCalls = targetDevice.callCount();
    return phase;
}

int sampledColorCount(const QImage &image)
{
    QSet<QRgb> colors;
    for (int y = 0; y < image.height(); y += 8) {
        for (int x = 0; x < image.width(); x += 8) {
            colors.insert(image.pixel(x, y));
        }
    }
    return colors.size();
}

int changedPixels(const QImage &first, const QImage &second)
{
    int changed = 0;
    for (int y = 0; y < first.height(); y += 3) {
        for (int x = 0; x < first.width(); x += 3) {
            const QColor before(first.pixel(x, y));
            const QColor after(second.pixel(x, y));
            if (std::abs(before.red() - after.red())
                    + std::abs(before.green() - after.green())
                    + std::abs(before.blue() - after.blue()) > 35) {
                ++changed;
            }
        }

    }
    return changed;
}

int nearColorPixels(
        const QImage &image,
        const QColor &target,
        const QRect &region,
        int tolerance = 10)
{
    int count = 0;
    const QRect bounded = region.intersected(image.rect());
    for (int y = bounded.top(); y <= bounded.bottom(); y += 2) {
        for (int x = bounded.left(); x <= bounded.right(); x += 2) {
            const QColor color(image.pixel(x, y));
            if (std::abs(color.red() - target.red()) <= tolerance
                    && std::abs(color.green() - target.green()) <= tolerance
                    && std::abs(color.blue() - target.blue()) <= tolerance) {
                ++count;
            }
        }
    }
    return count;
}

double horizontalDistanceToSegment(
        double pointX,
        double pointZ,
        double startX,
        double startZ,
        double endX,
        double endZ)
{
    const double segmentX = endX - startX;
    const double segmentZ = endZ - startZ;
    const double lengthSquared = segmentX * segmentX + segmentZ * segmentZ;
    const double projection = lengthSquared > 1.0e-9
            ? std::clamp(((pointX - startX) * segmentX
                    + (pointZ - startZ) * segmentZ) / lengthSquared,
                    0.0, 1.0)
            : 0.0;
    const double offsetX = pointX - (startX + projection * segmentX);
    const double offsetZ = pointZ - (startZ + projection * segmentZ);
    return std::hypot(offsetX, offsetZ);
}

double normalizedRadians(double angle)
{
    constexpr double pi = 3.14159265358979323846;
    while (angle > pi) angle -= 2.0 * pi;
    while (angle < -pi) angle += 2.0 * pi;
    return angle;
}

QQuickItem *findVisualItem(QQuickItem *parent, const QString &objectName)
{
    if (!parent) return nullptr;
    if (parent->objectName() == objectName) return parent;
    for (QQuickItem *child : parent->childItems()) {
        if (QQuickItem *match = findVisualItem(child, objectName)) return match;
    }
    return nullptr;
}

double tabletopApproachDistance(const WorkoutGameRoadCourse &road)
{
    for (const WorkoutGameRoadPiece &piece : road.pieces) {
        if (piece.terrain == WorkoutGameTerrainKind::Tabletop
                && piece.challenge.enabled) {
            return std::max(0.0,
                    piece.challenge.obstacleDistanceMeters - 5.0);
        }
    }
    return 64.0;
}

bool hasInteractiveGraphicsPlatform()
{
    const QString platform = QGuiApplication::platformName();
    return platform != QStringLiteral("offscreen")
            && platform != QStringLiteral("minimal");
}

}

class TestWorkoutGame3DView : public QObject
{
    Q_OBJECT

private slots:
    void rendererRequestsNonBlockingPresentation()
    {
        WorkoutGame3DWindow window(false);

        QCOMPARE(window.format().swapInterval(), 0);
    }

    void unchangedTelemetryDoesNotRepublishHudBindings()
    {
        WorkoutGame3DViewModel viewModel;
        QSignalSpy changed(
                &viewModel, &WorkoutGame3DViewModel::telemetryChanged);

        viewModel.setTelemetry(225.0, 230.0, 91, 155, 9);
        QCOMPARE(changed.count(), 1);
        for (int frame = 0; frame < 120; ++frame) {
            viewModel.setTelemetry(225.0, 230.0, 91, 155, 9);
        }
        QCOMPARE(changed.count(), 1);

        viewModel.setTelemetry(226.0, 230.0, 91, 155, 9);
        QCOMPARE(changed.count(), 2);
    }

    void climbUsesDedicatedGeometryAndBoundedEffortPoses()
    {
        const WorkoutGameCourse course = climbRenderCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const double effortDistance =
                piece->challenge.obstacleDistanceMeters - 9.0;
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);

        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        WorkoutGameVisualSnapshot effort = frameAt(road, effortDistance);
        effort.simulation.activeSection = 0;
        effort.simulation.sectionProgress = effortDistance
                / road.timeline.front().endDistanceMeters;
        effort.simulation.featureOutcome =
                WorkoutGameFeatureOutcome::Completed;
        effort.simulation.route = WorkoutGameRoute::MainLine;
        effort.feature = runtime.update(effort.simulation);
        for (int frame = 0; frame < 16; ++frame) {
            effort.simulation.workoutTimeMs = 1000 + frame * 100;
            viewModel.setFrame(effort, 250.0, 220.0, 54, 150, 5);
        }
        auto *geometry = qobject_cast<WorkoutGame3DGeometry *>(
                viewModel.climbGeometry());
        QVERIFY(geometry);
        QVERIFY(geometry->ready());
        QCOMPARE(geometry->sampleCount(), 680);
        QVERIFY(viewModel.riderStandingBlend() > 0.70);
        QVERIFY(viewModel.riderStandingBlend() <= 1.0);
        QVERIFY(!viewModel.riderWalking());
        const double standing = viewModel.riderStandingBlend();

        WorkoutGameVisualSnapshot crest = frameAt(
                road, piece->challenge.obstacleDistanceMeters);
        crest.simulation.activeSection = 0;
        crest.simulation.sectionProgress = 1.0;
        crest.simulation.featureOutcome =
                WorkoutGameFeatureOutcome::Completed;
        crest.simulation.route = WorkoutGameRoute::MainLine;
        crest.feature = runtime.update(crest.simulation);
        for (int frame = 0; frame < 12; ++frame) {
            crest.simulation.workoutTimeMs = 2700 + frame * 100;
            viewModel.setFrame(crest, 250.0, 220.0, 54, 150, 5);
        }
        QVERIFY(viewModel.riderStandingBlend() < standing * 0.15);

        effort.world.rider.walking = true;
        for (int frame = 0; frame < 12; ++frame) {
            effort.simulation.workoutTimeMs = 4000 + frame * 100;
            viewModel.setFrame(effort, 40.0, 220.0, 40, 145, 1);
        }
        QVERIFY(viewModel.riderWalking());
        QVERIFY(viewModel.riderStandingBlend() < 0.05);
    }

    void rendersClimbFaceCrestAndEmbeddedSteps()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const WorkoutGameCourse course = climbRenderCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
        });
        QVERIFY(piece != road.pieces.end());
        QCOMPARE(piece->challenge.profile.minimumEffortRatio, 1.0);
        const double distance =
                piece->challenge.obstacleDistanceMeters - 7.0;
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        WorkoutGameVisualSnapshot frame = frameAt(road, distance);
        frame.simulation.activeSection = 0;
        frame.simulation.sectionProgress = distance
                / road.timeline.front().endDistanceMeters;
        frame.simulation.featureOutcome =
                WorkoutGameFeatureOutcome::Completed;
        frame.simulation.route = WorkoutGameRoute::MainLine;
        frame.simulation.challenge = piece->challenge.profile;
        frame.simulation.challengeAssessment.effortReadiness = 1.0;
        frame.feature = runtime.update(frame.simulation);

        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.resize(960, 540);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        window.setCourse(course, FtpWatts);
        window.setFrame(frame, 250.0, 220.0, 54, 150, 5);
        QTest::qWait(500);
        auto *powerValue = window.rootObject()->findChild<QObject *>(
                QStringLiteral("featurePowerValue"));
        QVERIFY(powerValue);
        QCOMPARE(powerValue->property("text").toString(),
                 QStringLiteral("250 / 220 W"));
        QVERIFY(window.rootObject()->findChild<QObject *>(
                QStringLiteral("climbGeometryModel")));

        const QImage rendered = window.grabWindow();
        QVERIFY(!rendered.isNull());
        QCOMPARE(rendered.size(), QSize(960, 540));
        QVERIFY2(sampledColorCount(rendered) > 35,
                 "climb scene appears blank or nearly monochrome");
        const QString screenshot = qEnvironmentVariable(
                "GC_WORKOUT_GAME_CLIMB_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QVERIFY2(rendered.save(screenshot), qPrintable(screenshot));
        }
    }

    void missedClimbRendersNoBonusResult()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const WorkoutGameCourse course = climbRenderCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        WorkoutGameSimulation simulation;
        QVERIFY(simulation.configure(course, FtpWatts));
        WorkoutGameSimulationSnapshot simulationFrame;
        for (std::int64_t timeMs = 0; timeMs <= 30500; timeMs += 250) {
            WorkoutGameSimulationInput input;
            input.workoutTimeMs = timeMs;
            input.actualWatts = timeMs < 30000 ? 80.0 : 100.0;
            input.targetWatts = timeMs < 30000 ? 220.0 : 100.0;
            input.cadenceRpm = timeMs < 30000 ? 45.0 : 70.0;
            simulationFrame = simulation.update(input);
        }
        QCOMPARE(simulationFrame.activeSection, 1);
        QCOMPARE(simulationFrame.previousFeatureSection, 0);
        QCOMPARE(simulationFrame.previousFeatureOutcome,
                 WorkoutGameFeatureOutcome::Bypassed);
        const WorkoutGameRoadTimelineSection &runout = road.timeline[1];
        const double distance = runout.startDistanceMeters
                + (runout.endDistanceMeters - runout.startDistanceMeters)
                    * simulationFrame.sectionProgress;
        WorkoutGameVisualSnapshot frame = frameAt(road, distance);
        frame.simulation = simulationFrame;
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        frame.feature = runtime.update(frame.simulation);
        QCOMPARE(frame.feature.phase, WorkoutGameFeaturePhase::Recovery);
        QCOMPARE(frame.feature.terrain, WorkoutGameTerrainKind::Climb);
        QCOMPARE(frame.feature.outcome,
                 WorkoutGameFeatureOutcome::Bypassed);

        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.resize(960, 540);
        window.setCourse(course, FtpWatts);
        window.setFrame(frame, 100.0, 100.0, 70, 148, 3);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        auto *label = window.rootObject()->findChild<QObject *>(
                QStringLiteral("featureStateLabel"));
        QVERIFY(label);
        QTRY_COMPARE_WITH_TIMEOUT(label->property("text").toString(),
                QStringLiteral("NO BONUS"), 2000);
    }

    void exportsClimbSeatedStandingAndWalkingMotionFrames()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const QString outputRoot = qEnvironmentVariable(
                "GC_WORKOUT_GAME_CLIMB_VIDEO_DIR");
        if (outputRoot.isEmpty()) {
            QSKIP("Set GC_WORKOUT_GAME_CLIMB_VIDEO_DIR to export frames");
        }
        struct Scenario
        {
            const char *name;
            double watts;
            int cadence;
        };
        const std::array<Scenario, 3> scenarios = {{
            {"seated", 190.0, 82},
            {"standing", 250.0, 54},
            {"walking", 40.0, 40}
        }};
        const WorkoutGameCourse course = climbRenderCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.resize(960, 540);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        constexpr int FrameCount = 240;
        const double start = std::max(
                0.0, piece->challenge.obstacleDistanceMeters - 20.0);
        const double end = piece->challenge.obstacleDistanceMeters;

        for (const Scenario &scenario : scenarios) {
            const QString directory = QDir(outputRoot).filePath(
                    QString::fromLatin1(scenario.name));
            QVERIFY(QDir().mkpath(directory));
            window.setCourse(course, FtpWatts);
            WorkoutGamePhysics physics;
            QVERIFY(physics.configure(road));
            double maximumStanding = 0.0;
            double maximumLateral = 0.0;
            int walkingFrames = 0;
            int visiblyChangedFrames = 0;
            QImage previous;
            for (int frameIndex = 0; frameIndex < FrameCount; ++frameIndex) {
                const double progress = double(frameIndex)
                        / double(FrameCount - 1);
                const double distance = start + (end - start) * progress;
                const auto roadSample = WorkoutGameRoadCourseBuilder::sample(
                        road, distance);
                QVERIFY(roadSample.ready);
                WorkoutGameVisualSnapshot frame = frameAt(road, distance);
                frame.simulation.activeSection = 0;
                frame.simulation.sectionProgress = distance
                        / road.timeline.front().endDistanceMeters;
                frame.simulation.workoutTimeMs = frameIndex * 25;
                frame.simulation.route = WorkoutGameRoute::MainLine;
                frame.simulation.featureOutcome = scenario.watts >= 220.0
                        ? WorkoutGameFeatureOutcome::Completed
                        : WorkoutGameFeatureOutcome::Bypassed;
                frame.simulation.challenge = piece->challenge.profile;
                frame.simulation.challengeAssessment.effortReadiness =
                        std::clamp(scenario.watts / 220.0, 0.0, 1.0);
                frame.simulation.speedKph = 12.0;
                frame.feature = runtime.update(frame.simulation);
                WorkoutGamePhysicsInput input;
                input.workoutTimeMs = frame.simulation.workoutTimeMs;
                input.courseDistanceMeters = distance;
                input.terrain = roadSample.terrain;
                input.gradePercent = roadSample.center.gradePercent;
                input.difficulty = piece->difficulty;
                input.desiredSpeedMetersPerSecond = 3.33;
                input.effortRatio = scenario.watts / 220.0;
                frame.world = physics.update(input);
                QVERIFY(frame.world.ready);
                QVERIFY(!frame.world.rider.airborne);
                QCOMPARE(frame.feature.route, WorkoutGameRoute::MainLine);
                maximumLateral = std::max(maximumLateral,
                        std::abs(frame.feature.lateralOffsetMeters));
                if (frame.world.rider.walking) ++walkingFrames;

                window.setFrame(frame, scenario.watts, 220.0,
                        scenario.cadence, 150, 5);
                QTest::qWait(4);
                auto *viewModel = qobject_cast<WorkoutGame3DViewModel *>(
                        window.rootContext()->contextProperty(
                            QStringLiteral("workoutGame3D"))
                                .value<QObject *>());
                QVERIFY(viewModel);
                QVERIFY(viewModel->powerRequired());
                QCOMPARE(viewModel->requiredPowerWatts(), 220.0);
                maximumStanding = std::max(
                        maximumStanding, viewModel->riderStandingBlend());
                const QImage image = window.grabWindow();
                QVERIFY(!image.isNull());
                QVERIFY(sampledColorCount(image) > 30);
                if (!previous.isNull()
                        && changedPixels(previous, image) > 20) {
                    ++visiblyChangedFrames;
                }
                previous = image;
                const QString path = QDir(directory).filePath(
                        QStringLiteral("frame-%1.png")
                            .arg(frameIndex, 4, 10, QLatin1Char('0')));
                QImageWriter writer(path, "png");
                writer.setCompression(1);
                QVERIFY2(writer.write(image), qPrintable(writer.errorString()));
            }
            QCOMPARE(maximumLateral, 0.0);
            QVERIFY(visiblyChangedFrames > FrameCount * 4 / 5);
            if (QString::fromLatin1(scenario.name) == QStringLiteral("standing")) {
                QVERIFY(maximumStanding > 0.70);
            } else if (QString::fromLatin1(scenario.name)
                    == QStringLiteral("seated")) {
                QVERIFY(maximumStanding < 0.20);
            } else {
                QVERIFY(walkingFrames > FrameCount / 2);
                QVERIFY(maximumStanding < 0.05);
            }
        }
    }


    void skinnyUsesDedicatedGeometryAndBoundedDeterministicBalance()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Skinny);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const auto skinny = WorkoutGameSkinnyGeometry::profile(
                piece->difficulty);
        const double distance = piece->challenge.obstacleDistanceMeters - 1.4;
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        WorkoutGameVisualSnapshot frame = frameAt(road, distance);
        frame.simulation.activeSection = 0;
        frame.simulation.sectionProgress = std::clamp(
                distance / road.totalLengthMeters, 0.0, 1.0);
        frame.simulation.workoutTimeMs = 1000;
        frame.simulation.route = WorkoutGameRoute::MainLine;
        frame.simulation.featureOutcome =
                WorkoutGameFeatureOutcome::Completed;
        frame.simulation.challenge = piece->challenge.profile;
        WorkoutGameFeatureChallengeMetrics metrics;
        metrics.averageActualWatts = 180.0;
        metrics.averageTargetWatts = 175.0;
        metrics.averageEffortRatio = 180.0 / 175.0;
        metrics.averageCadenceRpm = 85.0;
        metrics.averageSpeedKph = 18.0;
        metrics.averageAdherence = 1.0;
        frame.simulation.challengeAssessment =
                WorkoutGameFeatureChallenge::assess(
                    frame.simulation.challenge, metrics);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        frame.feature = runtime.update(frame.simulation);
        viewModel.setFrame(frame, 180.0, 175.0, 85, 148, 5);

        auto *geometry = qobject_cast<WorkoutGame3DGeometry *>(
                viewModel.skinnyGeometry());
        QVERIFY(geometry);
        QTRY_VERIFY_WITH_TIMEOUT(
                qobject_cast<WorkoutGame3DGeometry *>(
                    viewModel.skinnyGeometry())->ready(), 3000);
        geometry = qobject_cast<WorkoutGame3DGeometry *>(
                viewModel.skinnyGeometry());
        QCOMPARE(geometry->sampleCount(), 1584);
        QVERIFY(viewModel.visibleTriangles() > 0);
        QVERIFY(viewModel.visibleTriangles() < 30000);
        QVERIFY(std::abs(viewModel.riderRoll()) > 0.2);
        QVERIFY(std::abs(viewModel.riderRoll()) <= 2.0);
        QCOMPARE(viewModel.riderRoll(),
                 skinny.balanceRollDegrees(-1.4));
        QCOMPARE(viewModel.featureActionText(), QStringLiteral("Balance"));
        const double cameraX = viewModel.cameraX();
        const double cameraY = viewModel.cameraY();
        const double cameraZ = viewModel.cameraZ();
        viewModel.setFrame(frame, 180.0, 175.0, 85, 148, 5);
        QCOMPARE(viewModel.cameraX(), cameraX);
        QCOMPARE(viewModel.cameraY(), cameraY);
        QCOMPARE(viewModel.cameraZ(), cameraZ);

        frame.simulation.route = WorkoutGameRoute::MainLine;
        frame.simulation.featureOutcome =
                WorkoutGameFeatureOutcome::Bypassed;
        frame.feature = runtime.update(frame.simulation);
        QCOMPARE(frame.feature.lateralOffsetMeters, 0.0);
        viewModel.setFrame(frame, 150.0, 175.0, 78, 145, 4);
        QCOMPARE(viewModel.riderRoll(), skinny.balanceRollDegrees(-1.4));
    }

    void initTestCase()
    {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    }

    void renderWorkStaysWithinInitialBudgets()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const ScopedEnvironmentVariable restoreStats(
                "GC_WORKOUT_GAME_RENDER_STATS");
        qputenv("GC_WORKOUT_GAME_RENDER_STATS", "1");
        const WorkoutGameCourse course = renderBudgetCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);

        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.setCourse(course, FtpWatts);
        window.resize(1280, 720);
        window.setSessionRunning(true);
        window.setFrame(
                frameAt(road, 70.0), 240.0, 220.0, 92, 152, 8);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);

        auto *viewModel = qobject_cast<WorkoutGame3DViewModel *>(
                window.rootContext()->contextProperty(
                    QStringLiteral("workoutGame3D")).value<QObject *>());
        QVERIFY(viewModel);
        QTRY_VERIFY_WITH_TIMEOUT(viewModel->visibleTriangles() > 0, 3000);
        QVERIFY(viewModel->visibleTriangles() < 30000);
        QVERIFY(viewModel->trees().size() <= 10);
        QVERIFY(viewModel->geometryQueueDepth() <= 1);

        QObject *view = window.rootObject()->findChild<QObject *>(
                QStringLiteral("workoutGame3DView"));
        QVERIFY(view);
        QObject *stats = view->property("renderStats").value<QObject *>();
        QVERIFY(stats);
        QTRY_VERIFY_WITH_TIMEOUT(
                stats->property("drawCallCount").toULongLong() > 0, 5000);
        QVERIFY2(stats->property("drawCallCount").toULongLong() <= 50,
                 qPrintable(QStringLiteral("draw-call budget exceeded: %1")
                         .arg(stats->property("drawCallCount")
                              .toULongLong())));
        window.setSessionRunning(false);
    }

    void productionWindowPublishesCompletedFrameDiagnostics()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const ScopedEnvironmentVariable restoreDiagnostics(
                "GC_WORKOUT_GAME_DIAGNOSTICS");
        qputenv("GC_WORKOUT_GAME_DIAGNOSTICS", "1");
        const WorkoutGameCourse course = sampleCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);

        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.setCourse(course, FtpWatts);
        window.resize(960, 540);
        WorkoutGameVisualSnapshot initial = frameAt(road, 18.0);
        initial.feature.ready = true;
        initial.feature.terrain = WorkoutGameTerrainKind::BunnyHop;
        initial.feature.phase = WorkoutGameFeaturePhase::Measure;
        initial.feature.outcome = WorkoutGameFeatureOutcome::Active;
        initial.feature.route = WorkoutGameRoute::MainLine;
        initial.feature.readiness = 0.73;
        initial.feature.distanceToObstacleMeters = 4.5;
        initial.feature.actionId = 17;
        initial.world.rider.rearWheelGrounded = true;
        initial.world.rider.frontWheelGrounded = false;
        window.setFrame(initial, 225.0, 220.0, 88, 149, 8);
        window.setSessionRunning(true);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        QObject *frameAnimation = window.rootObject()->findChild<QObject *>(
                QStringLiteral("presentationFrameAnimation"));
        QVERIFY(frameAnimation);
        QVERIFY(frameAnimation->property("running").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(
                window.diagnosticsSnapshot().ready
                && window.diagnosticsSnapshot().input.frameNumber > 2
                && window.diagnosticsSnapshot().input.framesPerSecond > 1.0
                && window.diagnosticsSnapshot().input
                    .p50FrameIntervalMs > 0.0
                && window.diagnosticsSnapshot().input
                    .p95FrameIntervalMs > 0.0
                && window.diagnosticsSnapshot().input
                    .p99FrameIntervalMs > 0.0,
                5000);
        const WorkoutGameDiagnosticsSnapshot first =
                window.diagnosticsSnapshot();
        QVERIFY(first.input.p50FrameIntervalMs
                <= first.input.p95FrameIntervalMs);
        QVERIFY(first.input.p95FrameIntervalMs
                <= first.input.p99FrameIntervalMs);
        QVERIFY(first.input.rendererQueueDepth <= 1);
        QVERIFY(first.input.presentationWorkMs >= 0.0);
        QVERIFY(first.largestPresentationWorkMs
                >= first.input.presentationWorkMs);
        const QString trace = window.diagnosticsTraceLine();
        QVERIFY(trace.startsWith(QStringLiteral("workout-game-3d-trace")));
        QVERIFY(trace.contains(QStringLiteral("p50_frame_ms=")));
        QVERIFY(trace.contains(QStringLiteral("p95_frame_ms=")));
        QVERIFY(trace.contains(QStringLiteral("p99_frame_ms=")));
        QVERIFY(trace.contains(QStringLiteral("frame_ms=")));
        QVERIFY(trace.contains(QStringLiteral("geometry_queue=")));
        QVERIFY(trace.contains(QStringLiteral("presentation_work_ms=")));
        QVERIFY(trace.contains(QStringLiteral("feature_phase=measure")));
        QVERIFY(trace.contains(QStringLiteral("feature_outcome=active")));
        QVERIFY(trace.contains(QStringLiteral("feature_terrain=bunny-hop")));
        QVERIFY(trace.contains(QStringLiteral("route=main")));
        QVERIFY(trace.contains(QStringLiteral("readiness=0.73")));
        QVERIFY(trace.contains(QStringLiteral("action_distance_m=4.5")));
        QVERIFY(trace.contains(QStringLiteral("action_id=17")));
        QVERIFY(trace.contains(QStringLiteral("rear_contact=1")));
        QVERIFY(trace.contains(QStringLiteral("front_contact=0")));
        QVERIFY(trace.contains(QStringLiteral("lateral_m=")));
        QVERIFY(trace.contains(QStringLiteral(
                "unexpected_airborne_frames=0")));
        QVERIFY(trace.contains(QStringLiteral("watts=225")));
        QVERIFY(trace.contains(QStringLiteral("target_watts=220")));
        QVERIFY(trace.contains(QStringLiteral("cadence=88")));
        QVERIFY(trace.contains(QStringLiteral("hr=149")));
        QVERIFY(trace.contains(QStringLiteral("gear=8")));
        QVERIFY(trace.contains(QStringLiteral("speed_kph=22.5")));
        QVERIFY(trace.contains(QStringLiteral("camera_pos=")));
        QVERIFY(trace.contains(QStringLiteral("camera_target=")));
        QVERIFY(trace.contains(QStringLiteral("rider_asset=RB-01")));
        QVERIFY(trace.contains(QStringLiteral("surface_asset=TR-08")));
        QVERIFY(trace.contains(QStringLiteral("near_environment=EN-01")));
        QVERIFY(trace.contains(QStringLiteral("distant_environment=EN-03")));
        QVERIFY(trace.contains(QStringLiteral("lod=resident")));
        QObject *diagnosticHud = window.rootObject()->findChild<QObject *>(
                QStringLiteral("diagnosticHud"));
        QObject *diagnosticText = window.rootObject()->findChild<QObject *>(
                QStringLiteral("diagnosticText"));
        QVERIFY(diagnosticHud);
        QVERIFY(diagnosticText);
        QVERIFY(diagnosticHud->property("visible").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(
                diagnosticText->property("text").toString().contains(
                    QStringLiteral("P50"))
                && diagnosticText->property("text").toString().contains(
                    QStringLiteral("P95"))
                && diagnosticText->property("text").toString().contains(
                    QStringLiteral("P99")),
                1000);
        const QString diagnosticScreenshot = qEnvironmentVariable(
                "GC_WORKOUT_GAME_3D_DIAGNOSTICS_SCREENSHOT");
        if (!diagnosticScreenshot.isEmpty()) {
            const QImage image = window.grabWindow();
            QVERIFY(!image.isNull());
            QVERIFY2(image.save(diagnosticScreenshot),
                     qPrintable(diagnosticScreenshot));
        }
        window.resize(360, 640);
        QTRY_COMPARE_WITH_TIMEOUT(window.rootObject()->width(), 360.0, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(window.rootObject()->height(), 640.0, 1000);
        auto *diagnosticItem = qobject_cast<QQuickItem *>(diagnosticHud);
        auto *trainingItem = window.rootObject()->findChild<QQuickItem *>(
                QStringLiteral("trainingHud"));
        QVERIFY(diagnosticItem);
        QVERIFY(trainingItem);
        QVERIFY(diagnosticItem->x() >= 0.0);
        QVERIFY(diagnosticItem->x() + diagnosticItem->width() <= 360.0);
        QVERIFY(diagnosticItem->y()
                >= trainingItem->y() + trainingItem->height());

        WorkoutGameVisualSnapshot advanced = frameAt(road, 24.0);
        advanced.simulation.workoutTimeMs += 1000;
        advanced.skippedSimulationTicks = 3;
        window.setFrame(advanced, 235.0, 230.0, 90, 151, 9);
        QTRY_VERIFY_WITH_TIMEOUT(
                window.diagnosticsSnapshot().input.skippedSimulationTicks
                    == std::size_t(3)
                && window.diagnosticsSnapshot().input
                    .renderedRoadDistanceMeters
                    > first.input.renderedRoadDistanceMeters,
                3000);
        window.setSessionRunning(false);
        QVERIFY(!frameAnimation->property("running").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!window.diagnosticsSnapshot().ready, 1000);
    }

    void targetGpuProtectsPresentationAndTrainingServiceBudgets()
    {
        if (qEnvironmentVariableIntValue(
                    "GC_WORKOUT_GAME_PERF_TEST") == 0) {
            QSKIP("Set GC_WORKOUT_GAME_PERF_TEST=1 on the target GPU");
        }
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const ScopedEnvironmentVariable restoreDiagnostics(
                "GC_WORKOUT_GAME_DIAGNOSTICS");
        qputenv("GC_WORKOUT_GAME_DIAGNOSTICS", "1");
        const WorkoutGameCourse course = renderBudgetCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);

        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.setCourse(course, FtpWatts);
        window.resize(1280, 720);
        const ServiceLatencyPhase baseline = measureServiceLatency(3000);
        QVERIFY(baseline.recordingOk);
        QVERIFY(baseline.trainerCalls >= 25);

        WorkoutGameVisualSnapshot initial = frameAt(road, 12.0);
        initial.presentationTimeMs = 1;
        initial.world.generation = 1;
        window.setFrame(initial, 220.0, 220.0, 88, 150, 7);
        window.setSessionRunning(true);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        QObject *frameAnimation = window.rootObject()->findChild<QObject *>(
                QStringLiteral("presentationFrameAnimation"));
        QVERIFY(frameAnimation);
        QVERIFY(frameAnimation->property("running").toBool());

        const ServiceLatencyPhase loaded = measureServiceLatency(
                8000, [&window, &road](qint64 elapsedMs) {
            const double residentSpan = std::max(
                    1.0, road.totalLengthMeters - 24.0);
            const double distance = 12.0 + std::fmod(
                    double(elapsedMs) * 0.006, residentSpan);
            WorkoutGameVisualSnapshot frame = frameAt(road, distance);
            frame.presentationTimeMs = elapsedMs + 2;
            frame.simulation.workoutTimeMs = elapsedMs;
            frame.world.generation = 1;
            window.setFrame(frame, 220.0, 220.0, 88, 150, 7);
        });
        const WorkoutGameDiagnosticsSnapshot diagnostics =
                window.diagnosticsSnapshot();
        window.setSessionRunning(false);
        QVERIFY(!frameAnimation->property("running").toBool());

        qInfo().noquote()
                << QStringLiteral(
                    "Workout Game target GPU performance: api=%1 "
                    "backend=%2 fps=%3 p50=%4 p95=%5 p99=%6 "
                    "trainer_baseline_p95=%7 trainer_loaded_p95=%8 "
                    "trainer_loaded_max=%9 trainer_missed=%10 "
                    "recording_baseline_p95=%11 recording_loaded_p95=%12 "
                    "recording_loaded_max=%13 recording_missed=%14 "
                    "skipped_ticks=%15 geometry_queue=%16")
                   .arg(int(window.rendererInterface()->graphicsApi()))
                   .arg(QQuickWindow::sceneGraphBackend())
                   .arg(diagnostics.input.framesPerSecond, 0, 'f', 1)
                   .arg(diagnostics.input.p50FrameIntervalMs, 0, 'f', 2)
                   .arg(diagnostics.input.p95FrameIntervalMs, 0, 'f', 2)
                   .arg(diagnostics.input.p99FrameIntervalMs, 0, 'f', 2)
                   .arg(baseline.trainer.percentile(0.95), 0, 'f', 1)
                   .arg(loaded.trainer.percentile(0.95), 0, 'f', 1)
                   .arg(loaded.trainer.maximum(), 0, 'f', 1)
                   .arg(loaded.trainer.missedDeadlines)
                   .arg(baseline.recording.percentile(0.95), 0, 'f', 1)
                   .arg(loaded.recording.percentile(0.95), 0, 'f', 1)
                   .arg(loaded.recording.maximum(), 0, 'f', 1)
                   .arg(loaded.recording.missedDeadlines)
                   .arg(diagnostics.input.skippedSimulationTicks)
                   .arg(diagnostics.input.rendererQueueDepth);
        qInfo().noquote()
                << "Workout Game target GPU service samples: trainer="
                << loaded.trainerCalls
                << "recording=" << loaded.recording.samples.size()
                << "telemetry=" << loaded.telemetry.samples.size()
                << "telemetry_p95=" << loaded.telemetry.percentile(0.95)
                << "telemetry_max=" << loaded.telemetry.maximum()
                << "telemetry_missed=" << loaded.telemetry.missedDeadlines
                << "screen_refresh="
                << (window.screen() ? window.screen()->refreshRate() : 0.0)
                << "mode="
                << (window.format().swapInterval() == 0
                    ? "nonblocking-swap" : "vsync");

        QVERIFY(loaded.recordingOk);
        QVERIFY(loaded.trainerCalls >= 70);
        QVERIFY(diagnostics.ready);
        QVERIFY2(diagnostics.input.framesPerSecond >= 55.0,
                 "target GPU did not sustain 55 presented frames per second");
        QVERIFY2(diagnostics.input.p95FrameIntervalMs <= 20.5,
                 "target GPU exceeded the 60 Hz p95 presentation budget");
        QVERIFY2(diagnostics.input.p99FrameIntervalMs <= 25.0,
                 "target GPU exceeded the bounded p99 presentation budget");
        QCOMPARE(diagnostics.input.skippedSimulationTicks, std::size_t(0));
        QVERIFY(diagnostics.input.rendererQueueDepth <= 1);
        QCOMPARE(loaded.trainer.missedDeadlines, 0);
        QCOMPARE(loaded.recording.missedDeadlines, 0);
        QCOMPARE(loaded.telemetry.missedDeadlines, 0);
        QVERIFY2(
                loaded.telemetry.percentile(0.95)
                    <= std::max(8.0,
                                baseline.telemetry.percentile(0.95) + 5.0),
                "rendering increased telemetry-event p95 scheduling latency");
        QVERIFY2(loaded.telemetry.maximum() <= 50.0,
                 "rendering blocked a telemetry-event deadline");
        QVERIFY2(
                loaded.trainer.percentile(0.95)
                    <= std::max(8.0,
                                baseline.trainer.percentile(0.95) + 5.0),
                "rendering increased trainer-target p95 scheduling latency");
        QVERIFY2(loaded.trainer.maximum() <= 50.0,
                 "rendering blocked a trainer-target deadline");
        QVERIFY2(
                loaded.recording.percentile(0.95)
                    <= std::max(12.0,
                                baseline.recording.percentile(0.95) + 8.0),
                "rendering increased recording p95 scheduling latency");
        QVERIFY2(loaded.recording.maximum() <= 75.0,
                 "rendering blocked a recording deadline");
    }

    void cameraCompositionDefaultsToCentreAndSupportsAuditVariants()
    {
        const ScopedEnvironmentVariable restore(
                "GC_WORKOUT_GAME_3D_CAMERA");

        qunsetenv("GC_WORKOUT_GAME_3D_CAMERA");
        {
            WorkoutGame3DViewModel medium;
            QCOMPARE(medium.cameraComposition(),
                     QStringLiteral("medium-centre"));
            QCOMPARE(medium.cameraSideMeters(), 0.0);
            QCOMPARE(medium.cameraBackMeters(), 8.2);
            QCOMPARE(medium.cameraHeightMeters(), 3.2);
            QCOMPARE(medium.cameraLookAheadMeters(), 12.0);
        }

        qputenv("GC_WORKOUT_GAME_3D_CAMERA", "low-centre");
        {
            WorkoutGame3DViewModel low;
            QCOMPARE(low.cameraComposition(), QStringLiteral("low-centre"));
            QCOMPARE(low.cameraSideMeters(), 0.0);
            QCOMPARE(low.cameraBackMeters(), 7.4);
            QCOMPARE(low.cameraHeightMeters(), 2.55);
            QCOMPARE(low.cameraTargetHeightMeters(), 0.75);
        }

        qputenv("GC_WORKOUT_GAME_3D_CAMERA", "shoulder");
        {
            WorkoutGame3DViewModel shoulder;
            QCOMPARE(shoulder.cameraComposition(), QStringLiteral("shoulder"));
            QVERIFY(shoulder.cameraSideMeters() > 0.0);
            QVERIFY(shoulder.cameraSideMeters() < 0.68);
        }

    }

    void cameraFollowsRoadAndMaintainsTerrainClearance()
    {
        const WorkoutGameCourse course = cameraMotionCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);

        for (double distance = 0.0;
             distance <= road.totalLengthMeters; distance += 1.0) {
            viewModel.setFrame(
                    frameAt(road, distance), 220.0, 220.0, 88, 150, 7);
            QVERIFY(std::isfinite(viewModel.cameraX()));
            QVERIFY(std::isfinite(viewModel.cameraY()));
            QVERIFY(std::isfinite(viewModel.cameraZ()));
            QVERIFY(std::isfinite(viewModel.cameraTargetX()));
            QVERIFY(std::isfinite(viewModel.cameraTargetY()));
            QVERIFY(std::isfinite(viewModel.cameraTargetZ()));

            const double cameraDistance = std::max(
                    0.0, distance - viewModel.cameraBackMeters());
            const WorkoutGameRoadSample cameraSample =
                    WorkoutGameRoadCourseBuilder::sample(
                            road, cameraDistance);
            QVERIFY(cameraSample.ready);
            const double cameraSurface =
                    cameraSample.center.elevationMeters
                    - cameraSample.nonPhysicalFeatureOffsetMeters;
            QVERIFY2(viewModel.cameraY() - cameraSurface >= 2.54,
                     "camera entered the terrain exclusion height");

            const double cameraToTarget = std::hypot(
                    viewModel.cameraTargetX() - viewModel.cameraX(),
                    viewModel.cameraTargetZ() - viewModel.cameraZ());
            QVERIFY2(cameraToTarget >= 10.0,
                     "camera target collapsed into the camera position");
            QVERIFY2(cameraToTarget <= 24.0,
                     "camera target escaped the bounded chase composition");
        }
    }

    void treesStayOutsideCameraAndCueCorridor()
    {
        const WorkoutGameCourse course = cameraMotionCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        int inspectedTrees = 0;

        for (double distance = 0.0;
             distance <= road.totalLengthMeters; distance += 0.5) {
            viewModel.setFrame(
                    frameAt(road, distance), 220.0, 220.0, 88, 150, 7);
            QVERIFY(viewModel.trees().size() <= 10);
            for (const QVariant &entry : viewModel.trees()) {
                const QVariantMap tree = entry.toMap();
                const double clearance = horizontalDistanceToSegment(
                        tree.value(QStringLiteral("x")).toDouble(),
                        tree.value(QStringLiteral("z")).toDouble(),
                        viewModel.cameraX(), viewModel.cameraZ(),
                        viewModel.cameraTargetX(), viewModel.cameraTargetZ());
                const double required = tree.value(
                        QStringLiteral("crownRadius")).toDouble() + 0.85;
                QVERIFY2(clearance + 1.0e-6 >= required,
                         qPrintable(QStringLiteral(
                             "tree clearance %1 is below required %2")
                             .arg(clearance).arg(required)));
                ++inspectedTrees;
            }
        }
        QVERIFY2(inspectedTrees >= 1000,
                 "camera exclusion removed the forest instead of relocating it");
    }

    void treesAreAnchoredToGeneratedTerrain()
    {
        const WorkoutGameCourse course = cameraMotionCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(frameAt(road, 30.0), 220.0, 220.0, 88, 150, 7);
        QVERIFY(!viewModel.trees().isEmpty());

        int reliefAnchors = 0;
        for (const QVariant &entry : viewModel.trees()) {
            const QVariantMap tree = entry.toMap();
            const double distance = tree.value(
                    QStringLiteral("distance")).toDouble();
            const double lateral = tree.value(
                    QStringLiteral("lateral")).toDouble();
            const WorkoutGameRoadSample sample =
                    WorkoutGameRoadCourseBuilder::sample(road, distance);
            const WorkoutGame3DTerrainProfileSnapshot terrain =
                    WorkoutGame3DTerrainProfile::build(
                        sample, distance, road.seed);
            QVERIFY(terrain.ready);
            const double expected =
                    WorkoutGame3DTerrainProfile::elevationAtLateral(
                        terrain, lateral);
            QVERIFY(std::abs(tree.value(QStringLiteral("y")).toDouble()
                             - expected) < 1.0e-9);
            if (std::abs(expected - sample.baseElevationMeters) > 0.20) {
                ++reliefAnchors;
            }
        }
        QVERIFY2(reliefAnchors > 0,
                 "trees still appear to use the former flat floor elevation");
    }

    void treesFadeAtTheResidentWindowEdges()
    {
        const WorkoutGameCourse course = cameraMotionCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(frameAt(road, 60.0), 205.0, 205.0, 86, 148, 6);

        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.resize(960, 540);
        window.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        window.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        QTest::qWait(500);

        const QList<QObject *> trees = window.rootObject()->findChildren<QObject *>(
                QStringLiteral("workoutGameTree"));
        QVERIFY(trees.size() >= 4);
        bool foundOpaqueNearTree = false;
        bool foundFadedEdgeTree = false;
        for (QObject *tree : trees) {
            const double relative = tree->property(
                    "relativeDistance").toDouble();
            const double opacity = tree->property("opacity").toDouble();
            QVERIFY(opacity >= 0.0);
            QVERIFY(opacity <= 1.0);
            if (relative >= -10.0 && relative <= 28.0
                    && opacity > 0.95) {
                foundOpaqueNearTree = true;
            }
            if ((relative < -12.0 || relative > 32.0)
                    && opacity < 0.90) {
                foundFadedEdgeTree = true;
            }
        }
        QVERIFY(foundOpaqueNearTree);
        QVERIFY(foundFadedEdgeTree);
    }

    void distantTerrainAndFogFollowTheBoundedSceneContract()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::SmoothTrail);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(frameAt(road, 8.0), 190.0, 190.0, 86, 148, 6);

        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.resize(960, 540);
        window.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        window.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        QObject *terrain = window.rootObject()->findChild<QObject *>(
                QStringLiteral("distantTerrainNode"));
        QObject *fog = window.rootObject()->findChild<QObject *>(
                QStringLiteral("workoutGameDepthFog"));
        QVERIFY(terrain);
        QVERIFY(fog);
        QCOMPARE(fog->property("enabled").toBool(), true);
        QCOMPARE(fog->property("depthEnabled").toBool(), true);
        QCOMPARE(fog->property("heightEnabled").toBool(), false);
        QCOMPARE(fog->property("depthNear").toDouble(), 68.0);
        QCOMPARE(fog->property("depthFar").toDouble(), 260.0);

        const auto verifyTerrainPosition = [&viewModel, terrain]() {
            QCoreApplication::processEvents();
            const QVector3D position =
                    terrain->property("position").value<QVector3D>();
            const QVector3D expected(
                    float(viewModel.riderX()),
                    float(viewModel.groundY() - 1.2),
                    float(viewModel.riderZ()));
            QVERIFY((position - expected).length() < 1.0e-4f);
        };
        verifyTerrainPosition();
        viewModel.setFrame(frameAt(road, 24.0), 195.0, 190.0, 87, 149, 6);
        verifyTerrainPosition();
    }

    void cameraMotionIsContinuousAndBounded()
    {
        const WorkoutGameCourse course = cameraMotionCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        constexpr int frameCount = 360;
        double priorX = 0.0;
        double priorY = 0.0;
        double priorZ = 0.0;
        double priorYaw = 0.0;
        double priorYawStep = 0.0;
        double maximumStep = 0.0;
        double maximumYawStep = 0.0;
        double maximumYawAcceleration = 0.0;

        for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
            const double progress = double(frameIndex)
                    / double(frameCount - 1);
            const double distance = 2.0
                    + (road.totalLengthMeters - 4.0) * progress;
            viewModel.setFrame(
                    frameAt(road, distance), 220.0, 220.0, 88, 150, 7);
            const double yaw = std::atan2(
                    viewModel.cameraTargetX() - viewModel.cameraX(),
                    viewModel.cameraTargetZ() - viewModel.cameraZ());
            if (frameIndex > 0) {
                maximumStep = std::max(maximumStep, std::sqrt(
                        std::pow(viewModel.cameraX() - priorX, 2.0)
                        + std::pow(viewModel.cameraY() - priorY, 2.0)
                        + std::pow(viewModel.cameraZ() - priorZ, 2.0)));
                const double yawStep = normalizedRadians(yaw - priorYaw);
                maximumYawStep = std::max(
                        maximumYawStep, std::abs(yawStep));
                if (frameIndex > 1) {
                    maximumYawAcceleration = std::max(
                            maximumYawAcceleration,
                            std::abs(normalizedRadians(
                                    yawStep - priorYawStep)));
                }
                priorYawStep = yawStep;
            }
            priorX = viewModel.cameraX();
            priorY = viewModel.cameraY();
            priorZ = viewModel.cameraZ();
            priorYaw = yaw;
        }

        QVERIFY2(maximumStep <= 0.55,
                 qPrintable(QStringLiteral("camera step reached %1 m")
                         .arg(maximumStep)));
        QVERIFY2(maximumYawStep <= 0.10,
                 qPrintable(QStringLiteral("camera yaw step reached %1 rad")
                         .arg(maximumYawStep)));
        QVERIFY2(maximumYawAcceleration <= 0.05,
                 qPrintable(QStringLiteral(
                         "camera yaw acceleration reached %1 rad/frame^2")
                         .arg(maximumYawAcceleration)));
    }

    void cameraPunctuationIsBoundedAndDoesNotMoveTheCameraRoot()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Tabletop);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const auto piece = std::find_if(
                road.pieces.cbegin(), road.pieces.cend(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.cend());

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        WorkoutGameVisualSnapshot frame = frameAt(
                road, piece->challenge.obstacleDistanceMeters - 1.0);
        frame.feature.ready = true;
        frame.feature.terrain = WorkoutGameTerrainKind::Tabletop;
        frame.feature.route = WorkoutGameRoute::MainLine;
        viewModel.setFrame(frame, 235.0, 220.0, 92, 152, 7);

        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.resize(960, 540);
        window.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        window.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        QObject *camera = window.rootObject()->findChild<QObject *>(
                QStringLiteral("workoutGameCamera"));
        QVERIFY(camera);

        const QString catalogDirectory = qEnvironmentVariable(
                "GC_WORKOUT_GAME_CAMERA_PUNCTUATION_DIR");
        if (!catalogDirectory.isEmpty()) {
            QVERIFY(QDir().mkpath(catalogDirectory));
            window.show();
            QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        }
        bool catalogSaveFailed = false;
        const auto capture = [&window, &catalogDirectory, &catalogSaveFailed](
                const QString &name) {
            if (catalogDirectory.isEmpty()) {
                return QImage();
            }
            window.update();
            QTest::qWait(180);
            const QImage image = window.grabWindow();
            const QString path = QDir(catalogDirectory).filePath(
                    name + QStringLiteral(".png"));
            if (image.isNull() || !image.save(path)) {
                catalogSaveFailed = true;
            }
            return image;
        };

        const auto verifyRoot = [&viewModel, camera]() {
            const QVector3D position =
                    camera->property("position").value<QVector3D>();
            const QVector3D expected(
                    float(viewModel.cameraX()),
                    float(viewModel.cameraY()),
                    float(viewModel.cameraZ()));
            QVERIFY((position - expected).length() < 1.0e-4f);
        };
        QTRY_VERIFY_WITH_TIMEOUT(
                std::abs(camera->property("fieldOfView").toDouble() - 47.0)
                    < 0.05, 1000);
        verifyRoot();
        const QImage baseline = capture(QStringLiteral("baseline"));

        frame.feature.phase = WorkoutGameFeaturePhase::Action;
        frame.feature.motion = WorkoutGameFeatureMotion::Jump;
        viewModel.setFrame(frame, 235.0, 220.0, 92, 152, 7);
        QCOMPARE(viewModel.riderPoseState(), QStringLiteral("preload"));
        QTRY_VERIFY_WITH_TIMEOUT(
                camera->property("fieldOfView").toDouble() < 46.5, 1000);
        QVERIFY(camera->property("fieldOfView").toDouble() >= 46.2);
        verifyRoot();
        const QImage preload = capture(QStringLiteral("preload"));

        frame.world.rider.airborne = true;
        frame.world.rider.clearanceMeters = 2.4;
        viewModel.setFrame(frame, 235.0, 220.0, 92, 152, 7);
        QCOMPARE(viewModel.riderPoseState(), QStringLiteral("air"));
        QTRY_VERIFY_WITH_TIMEOUT(
                camera->property("fieldOfView").toDouble() > 47.7, 1000);
        QVERIFY(camera->property("fieldOfView").toDouble() <= 48.4);
        verifyRoot();
        const QImage air = capture(QStringLiteral("air"));

        frame.world.rider.airborne = false;
        frame.world.rider.clearanceMeters = 0.0;
        frame.world.landingImpact = 1.0;
        viewModel.setFrame(frame, 235.0, 220.0, 92, 152, 7);
        QCOMPARE(viewModel.riderPoseState(), QStringLiteral("land"));
        QTRY_VERIFY_WITH_TIMEOUT(
                camera->property("fieldOfView").toDouble() > 47.7
                && camera->property("fieldOfView").toDouble() <= 48.2,
                1000);
        verifyRoot();
        const QImage landing = capture(QStringLiteral("landing"));

        frame.world.landingImpact = 0.0;
        frame.feature.route = WorkoutGameRoute::SafeBypass;
        viewModel.setFrame(frame, 180.0, 220.0, 78, 152, 7);
        QCOMPARE(viewModel.riderPoseState(), QStringLiteral("bypass"));
        QTRY_VERIFY_WITH_TIMEOUT(
                std::abs(camera->property("fieldOfView").toDouble() - 47.0)
                    < 0.05, 1000);
        verifyRoot();
        const QImage bypass = capture(QStringLiteral("bypass"));
        if (!catalogDirectory.isEmpty()) {
            QVERIFY(!catalogSaveFailed);
            const std::array<QImage, 5> images = {{
                baseline, preload, air, landing, bypass
            }};
            const std::array<QString, 5> names = {{
                QStringLiteral("baseline"),
                QStringLiteral("preload"),
                QStringLiteral("air"),
                QStringLiteral("landing"),
                QStringLiteral("bypass")
            }};
            for (std::size_t index = 0; index < images.size(); ++index) {
                QVERIFY(!images[index].isNull());
                QVERIFY(sampledColorCount(images[index]) > 35);
                const QString path = QDir(catalogDirectory).filePath(
                        names[index] + QStringLiteral(".png"));
                QVERIFY2(QFile::exists(path), qPrintable(path));
            }
            QVERIFY(changedPixels(baseline, preload) > 100);
            QVERIFY(changedPixels(baseline, air) > 100);
            QVERIFY(changedPixels(baseline, landing) > 100);
            QVERIFY(changedPixels(air, bypass) > 100);
        }
    }

    void rollerSuspensionDrivesTheRiderPumpPose()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Rollers);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        WorkoutGameVisualSnapshot frame = frameAt(
                road, piece->challenge.obstacleDistanceMeters);
        frame.world.terrain = WorkoutGameTerrainKind::Rollers;
        frame.world.rider.rearSuspension = 1.0;
        frame.world.rider.frontSuspension = 1.0;
        viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
        QCOMPARE(viewModel.riderPump(), -0.10);

        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.resize(1280, 720);
        window.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        window.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        QCoreApplication::processEvents();
        QObject *body = window.rootObject()->findChild<QObject *>(
                QStringLiteral("riderBodyNode"));
        QVERIFY(body);
        QVERIFY(std::abs(body->property("y").toDouble() - 1.13) < 1.0e-6);

        frame.world.rider.rearSuspension = 0.0;
        frame.world.rider.frontSuspension = 0.0;
        frame.world.rider.distanceMeters =
                piece->challenge.obstacleDistanceMeters + 1.5;
        viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
        QCOMPARE(viewModel.riderPump(), 0.06);

        frame.feature.route = WorkoutGameRoute::SafeBypass;
        viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
        QCOMPARE(viewModel.riderPump(), 0.0);
    }

    void authoredRiderWheelsAndDrivetrainFollowDistanceAndCadence()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::SmoothTrail);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(frameAt(road, 8.0), 190.0, 190.0, 86, 148, 6);

        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.resize(960, 540);
        window.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        window.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        QCoreApplication::processEvents();

        QObject *rearWheel = window.rootObject()->findChild<QObject *>(
                QStringLiteral("rearWheelPivot"));
        QObject *frontWheel = window.rootObject()->findChild<QObject *>(
                QStringLiteral("frontWheelPivot"));
        QObject *crank = window.rootObject()->findChild<QObject *>(
                QStringLiteral("crankPivot"));
        QObject *lowerLeg = window.rootObject()->findChild<QObject *>(
                QStringLiteral("leftLowerLeg"));
        QObject *riderTexture = window.rootObject()->findChild<QObject *>(
                QStringLiteral("riderPixelTexture"));
        QVERIFY(rearWheel);
        QVERIFY(frontWheel);
        QVERIFY(crank);
        QVERIFY(lowerLeg);
        QVERIFY(riderTexture);
        for (const QString &materialName : {
                 QStringLiteral("riderBikeMaterial"),
                 QStringLiteral("riderJerseyMaterial"),
                 QStringLiteral("riderShortsMaterial"),
                 QStringLiteral("riderHelmetMaterial")}) {
            QObject *material = window.rootObject()->findChild<QObject *>(
                    materialName);
            QVERIFY2(material, qPrintable(materialName));
            QCOMPARE(material->property("baseColorMap").value<QObject *>(),
                     riderTexture);
        }
        const QVector3D firstRear =
                rearWheel->property("eulerRotation").value<QVector3D>();
        const QVector3D firstFront =
                frontWheel->property("eulerRotation").value<QVector3D>();
        const QVector3D firstCrank =
                crank->property("eulerRotation").value<QVector3D>();
        const QVector3D firstLeg =
                lowerLeg->property("eulerRotation").value<QVector3D>();

        viewModel.setFrame(frameAt(road, 9.0), 190.0, 190.0, 86, 148, 6);
        QCoreApplication::processEvents();
        const QVector3D secondRear =
                rearWheel->property("eulerRotation").value<QVector3D>();
        const QVector3D secondFront =
                frontWheel->property("eulerRotation").value<QVector3D>();
        const QVector3D secondCrank =
                crank->property("eulerRotation").value<QVector3D>();
        const QVector3D secondLeg =
                lowerLeg->property("eulerRotation").value<QVector3D>();
        QVERIFY(std::abs(secondRear.x() - firstRear.x()) > 1.0);
        QCOMPARE(secondRear.x(), secondFront.x());
        QCOMPARE(firstRear.x(), firstFront.x());
        QVERIFY(std::abs(secondCrank.x() - firstCrank.x()) > 1.0);
        QVERIFY((secondLeg - firstLeg).length() > 0.5f);
    }

    void riderActionStateFollowsAuthoritativeSnapshots()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::SmoothTrail);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);

        auto frame = frameAt(road, 8.0);
        viewModel.setFrame(frame, 190.0, 190.0, 86, 148, 6);
        QCOMPARE(viewModel.riderPoseState(), QStringLiteral("pedal"));

        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.resize(960, 540);
        window.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        window.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        const QString catalogDirectory = qEnvironmentVariable(
                "GC_WORKOUT_GAME_RIDER_ACTION_CATALOG");
        const bool renderCatalog = !catalogDirectory.isEmpty()
                && hasInteractiveGraphicsPlatform();
        if (renderCatalog) {
            QVERIFY(QDir().mkpath(catalogDirectory));
            window.show();
            QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        }
        QObject *rider = window.rootObject()->findChild<QObject *>(
                QStringLiteral("riderNode"));
        QVERIFY(rider);
        QImage previousImage;
        const auto verifyPose = [
                &viewModel, &window, rider, renderCatalog,
                &catalogDirectory, &previousImage](const QString &expected) {
            QCOMPARE(viewModel.riderPoseState(), expected);
            QCoreApplication::processEvents();
            QCOMPARE(rider->property("poseState").toString(), expected);
            QTest::qWait(140);
            QVERIFY(rider->property("actionHeight").toDouble() >= -0.12);
            QVERIFY(rider->property("actionHeight").toDouble() <= 0.07);
            QVERIFY(rider->property("actionPitch").toDouble() >= -9.0);
            QVERIFY(rider->property("actionPitch").toDouble() <= 11.0);
            QVERIFY(rider->property("coastBlend").toDouble() >= 0.0);
            QVERIFY(rider->property("coastBlend").toDouble() <= 1.0);
            const QVector3D rootPosition =
                    rider->property("position").value<QVector3D>();
            const QVector3D physicsPosition(
                    float(viewModel.riderX()),
                    float(viewModel.riderY()),
                    float(viewModel.riderZ()));
            QVERIFY((rootPosition - physicsPosition).length() < 1.0e-4f);
            if (renderCatalog) {
                const QImage image = window.grabWindow();
                QVERIFY(!image.isNull());
                QVERIFY(sampledColorCount(image) > 45);
                if (!previousImage.isNull()) {
                    QVERIFY2(changedPixels(previousImage, image) > 3,
                             qPrintable(expected));
                }
                if (!catalogDirectory.isEmpty()) {
                    QVERIFY(image.save(QDir(catalogDirectory).filePath(
                            expected + QStringLiteral(".png"))));
                }
                previousImage = image;
            }
        };
        verifyPose(QStringLiteral("pedal"));

        viewModel.setFrame(frame, 0.0, 190.0, 0, 148, 6);
        verifyPose(QStringLiteral("coast"));

        frame.feature.motion = WorkoutGameFeatureMotion::Jump;
        frame.feature.phase = WorkoutGameFeaturePhase::Action;
        frame.feature.ready = true;
        viewModel.setFrame(frame, 230.0, 220.0, 90, 150, 7);
        verifyPose(QStringLiteral("preload"));

        frame.world.rider.airborne = true;
        frame.world.rider.clearanceMeters = 1.42;
        viewModel.setFrame(frame, 0.0, 220.0, 0, 150, 7);
        verifyPose(QStringLiteral("air"));

        frame.world.rider.airborne = false;
        frame.world.landingImpact = 0.8;
        viewModel.setFrame(frame, 0.0, 220.0, 0, 150, 7);
        verifyPose(QStringLiteral("land"));

        frame.world.landingImpact = 0.0;
        frame.feature.motion = WorkoutGameFeatureMotion::Absorb;
        viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
        verifyPose(QStringLiteral("absorb"));

        frame.feature = {};
        frame.world.rider.rollDegrees = 8.0;
        viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
        verifyPose(QStringLiteral("lean"));

        frame.world.rider.rollDegrees = 0.0;
        frame.feature.route = WorkoutGameRoute::SafeBypass;
        viewModel.setFrame(frame, 180.0, 220.0, 80, 150, 5);
        verifyPose(QStringLiteral("bypass"));
    }

    void landingAndSuccessEffectsUseBoundedAuthoritativeEvents()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Tabletop);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const auto piece = std::find_if(
                road.pieces.cbegin(), road.pieces.cend(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.cend());

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        WorkoutGameVisualSnapshot frame = frameAt(
                road, piece->challenge.obstacleDistanceMeters + 1.0);
        frame.feature.ready = true;
        frame.feature.terrain = WorkoutGameTerrainKind::Tabletop;
        frame.feature.phase = WorkoutGameFeaturePhase::Action;
        frame.feature.outcome = WorkoutGameFeatureOutcome::None;
        frame.feature.actionId = 41;
        viewModel.setFrame(frame, 235.0, 220.0, 92, 152, 7);

        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.resize(960, 540);
        window.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        window.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);

        QObject *dust = window.rootObject()->findChild<QObject *>(
                QStringLiteral("landingDustBurst"));
        QObject *success = window.rootObject()->findChild<QObject *>(
                QStringLiteral("successFeedback"));
        QObject *successText = window.rootObject()->findChild<QObject *>(
                QStringLiteral("successFeedbackText"));
        QObject *dustAnimation = window.rootObject()->findChild<QObject *>(
                QStringLiteral("landingDustAnimation"));
        QObject *successAnimation = window.rootObject()->findChild<QObject *>(
                QStringLiteral("successFeedbackAnimation"));
        auto *successItem = qobject_cast<QQuickItem *>(success);
        auto *dustItem = qobject_cast<QQuickItem *>(dust);
        auto *trainingHud = window.rootObject()->findChild<QQuickItem *>(
                QStringLiteral("trainingHud"));
        QVERIFY(dust);
        QVERIFY(success);
        QVERIFY(successText);
        QVERIFY(dustAnimation);
        QVERIFY(successAnimation);
        QVERIFY(successItem);
        QVERIFY(dustItem);
        QVERIFY(trainingHud);
        QVERIFY(successItem->y() >= trainingHud->y()
                + trainingHud->height() + 4.0);
        QVERIFY(window.rootObject()->findChild<QObject *>(
                QStringLiteral("landingDustLeft")));
        QVERIFY(window.rootObject()->findChild<QObject *>(
                QStringLiteral("landingDustCentre")));
        QVERIFY(window.rootObject()->findChild<QObject *>(
                QStringLiteral("landingDustRight")));
        const QString outputDirectory = qEnvironmentVariable(
                "GC_WORKOUT_GAME_FX_CATALOG_DIR");
        QImage baseline;
        QImage landing;
        QImage settled;
        QImage completed;
        if (!outputDirectory.isEmpty()) {
            QVERIFY(QDir().mkpath(outputDirectory));
            baseline = window.grabWindow();
            QVERIFY(!baseline.isNull());
        }

        const qulonglong initialLanding =
                viewModel.property("landingEffectId").toULongLong();
        frame.world.landingImpact = 0.72;
        viewModel.setFrame(frame, 235.0, 220.0, 92, 152, 7);
        QCOMPARE(viewModel.property("landingEffectId").toULongLong(),
                 initialLanding + 1);
        QCOMPARE(viewModel.property("landingEffectStrength").toDouble(), 0.72);
        QCOMPARE(dust->property("triggerId").toULongLong(),
                 initialLanding + 1);
        QTRY_VERIFY_WITH_TIMEOUT(dust->property("opacity").toDouble() > 0.05,
                                 500);
        QCOMPARE(dust->property("capturedStrength").toDouble(), 0.72);
        const double dustCentreX = dustItem->x() + dustItem->width() / 2.0;
        const double dustCentreY = dustItem->y() + dustItem->height() / 2.0;
        QVERIFY(dustCentreX > window.width() * 0.20);
        QVERIFY(dustCentreX < window.width() * 0.80);
        QVERIFY(dustCentreY > trainingHud->y() + trainingHud->height());
        QVERIFY(dustCentreY < window.height() * 0.80);
        if (!outputDirectory.isEmpty()) {
            QVERIFY(dustAnimation->setProperty("running", false));
            QVERIFY(dust->setProperty("progress", 0.25));
            QVERIFY(dust->property("opacity").toDouble() > 0.70);
            window.update();
            QTest::qWait(50);
            window.grabWindow();
            window.update();
            QTest::qWait(50);
            landing = window.grabWindow();
            QVERIFY(!landing.isNull());
            QVERIFY(changedPixels(baseline, landing) > 8);
            QVERIFY(dust->setProperty("progress", 1.0));
        }

        viewModel.setFrame(frame, 235.0, 220.0, 92, 152, 7);
        QCOMPARE(viewModel.property("landingEffectId").toULongLong(),
                 initialLanding + 1);
        frame.world.landingImpact = 0.0;
        viewModel.setFrame(frame, 235.0, 220.0, 92, 152, 7);
        QTRY_VERIFY_WITH_TIMEOUT(dust->property("opacity").toDouble() < 0.01,
                                 1000);
        if (!outputDirectory.isEmpty()) {
            window.update();
            QTest::qWait(50);
            window.grabWindow();
            window.update();
            QTest::qWait(50);
            settled = window.grabWindow();
            QVERIFY(!settled.isNull());
        }

        const qulonglong initialSuccess =
                viewModel.property("successEffectId").toULongLong();
        frame.world.landingImpact = 0.0;
        frame.feature.phase = WorkoutGameFeaturePhase::Recovery;
        frame.feature.outcome = WorkoutGameFeatureOutcome::Completed;
        frame.feature.actionId = 101;
        viewModel.setFrame(frame, 235.0, 220.0, 92, 152, 7);
        QCOMPARE(viewModel.property("successEffectId").toULongLong(),
                 initialSuccess + 1);
        QCOMPARE(success->property("triggerId").toULongLong(),
                 initialSuccess + 1);
        QTRY_VERIFY_WITH_TIMEOUT(success->property("opacity").toDouble() > 0.05,
                                 500);
        QTRY_COMPARE_WITH_TIMEOUT(successText->property("text").toString(),
                                  QStringLiteral("TABLETOP CLEAN"), 500);
        if (!outputDirectory.isEmpty()) {
            QVERIFY(successAnimation->setProperty("running", false));
            QVERIFY(success->setProperty("progress", 0.22));
            window.update();
            QTest::qWait(50);
            window.grabWindow();
            window.update();
            QTest::qWait(50);
            completed = window.grabWindow();
            QVERIFY(!completed.isNull());
            QVERIFY(changedPixels(settled, completed) > 20);
            QVERIFY(success->setProperty("progress", 1.0));
        }

        viewModel.setFrame(frame, 235.0, 220.0, 92, 152, 7);
        QCOMPARE(viewModel.property("successEffectId").toULongLong(),
                 initialSuccess + 1);
        frame.feature.outcome = WorkoutGameFeatureOutcome::Bypassed;
        frame.feature.actionId = 102;
        viewModel.setFrame(frame, 180.0, 220.0, 78, 152, 7);
        QCOMPARE(viewModel.property("successEffectId").toULongLong(),
                 initialSuccess + 1);
        QCoreApplication::processEvents();
        QVERIFY(successAnimation->setProperty("running", false));
        QVERIFY(success->setProperty("progress", 1.0));
        QCoreApplication::processEvents();

        frame.feature.phase = WorkoutGameFeaturePhase::Recovery;
        frame.feature.outcome = WorkoutGameFeatureOutcome::Completed;
        frame.feature.actionId = 103;
        frame.world.landingImpact = 0.40;
        viewModel.setFrame(frame, 235.0, 220.0, 92, 152, 7);
        QCOMPARE(viewModel.property("landingEffectId").toULongLong(),
                 initialLanding + 2);
        QCOMPARE(viewModel.property("successEffectId").toULongLong(),
                 initialSuccess + 2);
        QTRY_VERIFY_WITH_TIMEOUT(dust->property("opacity").toDouble() > 0.05,
                                 500);
        QTRY_VERIFY_WITH_TIMEOUT(success->property("opacity").toDouble() > 0.05,
                                 500);
        if (!outputDirectory.isEmpty()) {
            const QString motionDirectory = QDir(outputDirectory).filePath(
                    QStringLiteral("landing-success-motion"));
            QVERIFY(QDir().mkpath(motionDirectory));
            for (int index = 0; index < 16; ++index) {
                window.update();
                QTest::qWait(30);
                const QImage image = window.grabWindow();
                QVERIFY(!image.isNull());
                const QString path = QDir(motionDirectory).filePath(
                        QStringLiteral("frame-%1.png")
                            .arg(index, 3, 10, QLatin1Char('0')));
                QVERIFY2(image.save(path), qPrintable(path));
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(dust->property("opacity").toDouble() < 0.01,
                                 1000);

        if (!outputDirectory.isEmpty()) {
            QVERIFY(baseline.save(QDir(outputDirectory).filePath(
                    QStringLiteral("baseline.png"))));
            QVERIFY(landing.save(QDir(outputDirectory).filePath(
                    QStringLiteral("landing-dust.png"))));
            QVERIFY(completed.save(QDir(outputDirectory).filePath(
                    QStringLiteral("success-feedback.png"))));
        }

        window.resize(360, 640);
        QTRY_COMPARE_WITH_TIMEOUT(window.rootObject()->width(), 360.0, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(window.rootObject()->height(), 640.0, 1000);
        QVERIFY(successItem->x() >= 0.0);
        QVERIFY(successItem->x() + successItem->width() <= 360.0);
        QVERIFY(successItem->y() >= trainingHud->y()
                + trainingHud->height() + 4.0);
        auto *featureHud = window.rootObject()->findChild<QQuickItem *>(
                QStringLiteral("featureHud"));
        QVERIFY(featureHud);
        QVERIFY(successItem->y() + successItem->height()
                < featureHud->y() - 4.0);
        QTRY_VERIFY_WITH_TIMEOUT(dustItem->x() >= 0.0, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(
                dustItem->x() + dustItem->width() <= 360.0, 1000);

        QQuickView reloaded;
        reloaded.setResizeMode(QQuickView::SizeRootObjectToView);
        reloaded.resize(960, 540);
        reloaded.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        reloaded.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(reloaded.status(), QQuickView::Ready);
        QCoreApplication::processEvents();
        QObject *reloadedDust = reloaded.rootObject()->findChild<QObject *>(
                QStringLiteral("landingDustBurst"));
        QObject *reloadedSuccess = reloaded.rootObject()->findChild<QObject *>(
                QStringLiteral("successFeedback"));
        QVERIFY(reloadedDust);
        QVERIFY(reloadedSuccess);
        QCOMPARE(reloadedDust->property("opacity").toDouble(), 0.0);
        QCOMPARE(reloadedSuccess->property("opacity").toDouble(), 0.0);
    }

    void rootsUseFilteredSuspensionMotionWithoutCameraVibration()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Roots);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        WorkoutGameVisualSnapshot frame = frameAt(
                road, piece->challenge.obstacleDistanceMeters - 1.0);
        frame.world.terrain = WorkoutGameTerrainKind::Roots;
        frame.world.rider.rearSuspension = 0.2;
        frame.world.rider.frontSuspension = 0.2;
        frame.simulation.workoutTimeMs = 1000;
        frame.simulation.activeSection = 0;
        frame.simulation.sectionProgress = std::clamp(
                frame.world.rider.distanceMeters / road.totalLengthMeters,
                0.0, 1.0);
        frame.simulation.challenge = piece->challenge.profile;
        QCOMPARE(frame.simulation.challenge.minimumEffortRatio, 1.0);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        frame.feature = runtime.update(frame.simulation);
        viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
        QVERIFY(viewModel.powerRequired());
        QCOMPARE(viewModel.requiredPowerWatts(), 220.0);
        auto *rootsGeometry = qobject_cast<WorkoutGame3DGeometry *>(
                viewModel.rootsGeometry());
        QVERIFY(rootsGeometry);
        QTRY_VERIFY_WITH_TIMEOUT(
                qobject_cast<WorkoutGame3DGeometry *>(
                    viewModel.rootsGeometry())->ready(), 3000);
        rootsGeometry = qobject_cast<WorkoutGame3DGeometry *>(
                viewModel.rootsGeometry());
        QVERIFY(rootsGeometry->sampleCount() >= 40);
        QVERIFY(viewModel.visibleTriangles() > 0);
        QVERIFY(viewModel.visibleTriangles() < 30000);
        QCOMPARE(viewModel.riderPump(), 0.0);

        const double cameraX = viewModel.cameraX();
        const double cameraY = viewModel.cameraY();
        const double cameraZ = viewModel.cameraZ();
        frame.world.rider.rearSuspension = 1.0;
        frame.world.rider.frontSuspension = 1.0;
        frame.simulation.workoutTimeMs = 1080;
        viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
        QVERIFY(viewModel.riderPump() < -0.01);
        QVERIFY(viewModel.riderPump() >= -0.05);
        QCOMPARE(viewModel.cameraX(), cameraX);
        QCOMPARE(viewModel.cameraY(), cameraY);
        QCOMPARE(viewModel.cameraZ(), cameraZ);

        frame.feature.route = WorkoutGameRoute::SafeBypass;
        frame.simulation.workoutTimeMs = 1160;
        viewModel.setFrame(frame, 180.0, 220.0, 80, 145, 7);
        QCOMPARE(viewModel.riderPump(), 0.0);
    }

    void rockGardenUsesFilteredSuspensionMotionWithoutCameraVibration()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::RockGarden);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        WorkoutGameVisualSnapshot frame = frameAt(
                road, piece->challenge.obstacleDistanceMeters - 1.0);
        frame.world.terrain = WorkoutGameTerrainKind::RockGarden;
        frame.world.rider.rearSuspension = 0.2;
        frame.world.rider.frontSuspension = 0.2;
        frame.simulation.workoutTimeMs = 1000;
        frame.simulation.activeSection = 0;
        frame.simulation.sectionProgress = std::clamp(
                frame.world.rider.distanceMeters / road.totalLengthMeters,
                0.0, 1.0);
        frame.simulation.challenge = piece->challenge.profile;
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        frame.feature = runtime.update(frame.simulation);
        viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
        QVERIFY(viewModel.powerRequired());
        auto *rocksGeometry = qobject_cast<WorkoutGame3DGeometry *>(
                viewModel.rockGardenGeometry());
        QVERIFY(rocksGeometry);
        QTRY_VERIFY_WITH_TIMEOUT(
                qobject_cast<WorkoutGame3DGeometry *>(
                    viewModel.rockGardenGeometry())->ready(), 3000);
        rocksGeometry = qobject_cast<WorkoutGame3DGeometry *>(
                viewModel.rockGardenGeometry());
        QCOMPARE(rocksGeometry->sampleCount(), 12 * 15);
        QVERIFY(viewModel.visibleTriangles() > 0);
        QVERIFY(viewModel.visibleTriangles() < 30000);
        QCOMPARE(viewModel.riderPump(), 0.0);

        const double cameraX = viewModel.cameraX();
        const double cameraY = viewModel.cameraY();
        const double cameraZ = viewModel.cameraZ();
        frame.world.rider.rearSuspension = 1.0;
        frame.world.rider.frontSuspension = 1.0;
        frame.simulation.workoutTimeMs = 1070;
        viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
        QVERIFY(viewModel.riderPump() < -0.015);
        QVERIFY(viewModel.riderPump() >= -0.08);
        QCOMPARE(viewModel.cameraX(), cameraX);
        QCOMPARE(viewModel.cameraY(), cameraY);
        QCOMPARE(viewModel.cameraZ(), cameraZ);

        frame.feature.route = WorkoutGameRoute::SafeBypass;
        frame.simulation.workoutTimeMs = 1140;
        viewModel.setFrame(frame, 170.0, 220.0, 78, 145, 4);
        QCOMPARE(viewModel.riderPump(), 0.0);
    }

    void rockSlabUsesFilteredSuspensionMotionWithoutCameraVibration()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::RockSlab);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        WorkoutGameVisualSnapshot frame = frameAt(
                road, piece->challenge.obstacleDistanceMeters - 1.0);
        frame.world.terrain = WorkoutGameTerrainKind::RockSlab;
        frame.world.rider.rearSuspension = 0.2;
        frame.world.rider.frontSuspension = 0.2;
        frame.simulation.workoutTimeMs = 1000;
        frame.simulation.activeSection = 0;
        frame.simulation.sectionProgress = std::clamp(
                frame.world.rider.distanceMeters / road.totalLengthMeters,
                0.0, 1.0);
        frame.simulation.challenge = piece->challenge.profile;
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        frame.feature = runtime.update(frame.simulation);
        viewModel.setFrame(frame, 225.0, 220.0, 88, 150, 7);
        QVERIFY(viewModel.powerRequired());
        auto *slabGeometry = qobject_cast<WorkoutGame3DGeometry *>(
                viewModel.rockSlabGeometry());
        QVERIFY(slabGeometry);
        QTRY_VERIFY_WITH_TIMEOUT(
                qobject_cast<WorkoutGame3DGeometry *>(
                    viewModel.rockSlabGeometry())->ready(), 3000);
        slabGeometry = qobject_cast<WorkoutGame3DGeometry *>(
                viewModel.rockSlabGeometry());
        QCOMPARE(slabGeometry->sampleCount(), 147);
        QVERIFY(viewModel.visibleTriangles() > 0);
        QVERIFY(viewModel.visibleTriangles() < 30000);
        QCOMPARE(viewModel.riderPump(), 0.0);

        const double cameraX = viewModel.cameraX();
        const double cameraY = viewModel.cameraY();
        const double cameraZ = viewModel.cameraZ();
        frame.world.rider.rearSuspension = 1.0;
        frame.world.rider.frontSuspension = 1.0;
        frame.simulation.workoutTimeMs = 1075;
        viewModel.setFrame(frame, 225.0, 220.0, 88, 150, 7);
        QVERIFY(viewModel.riderPump() < -0.01);
        QVERIFY(viewModel.riderPump() >= -0.07);
        QCOMPARE(viewModel.cameraX(), cameraX);
        QCOMPARE(viewModel.cameraY(), cameraY);
        QCOMPARE(viewModel.cameraZ(), cameraZ);

        frame.feature.route = WorkoutGameRoute::SafeBypass;
        frame.simulation.workoutTimeMs = 1150;
        viewModel.setFrame(frame, 170.0, 220.0, 78, 145, 4);
        QCOMPARE(viewModel.riderPump(), 0.0);
    }

    void bermRiderRollUsesTheRoadBankAndNotASeparateAnimation()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Berm);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const double distance = piece->challenge.obstacleDistanceMeters;
        const WorkoutGameRoadSample sample =
                WorkoutGameRoadCourseBuilder::sample(road, distance);
        QVERIFY(sample.ready);
        QVERIFY(std::abs(sample.bermBankRadians) > 0.30);

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        auto *bermGeometry = qobject_cast<WorkoutGame3DGeometry *>(
                viewModel.bermGeometry());
        QVERIFY(bermGeometry);
        QVERIFY(bermGeometry->ready());
        WorkoutGameVisualSnapshot frame = frameAt(road, distance);
        frame.world.terrain = WorkoutGameTerrainKind::Berm;
        frame.world.rider.rollDegrees = 0.0;
        frame.simulation.speedKph = 20.0;
        frame.feature.route = WorkoutGameRoute::MainLine;
        viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
        const double mainRoll = viewModel.riderRoll();
        QVERIFY(std::abs(mainRoll) > 15.0);
        QVERIFY(viewModel.riderRoll() * sample.bermBankRadians > 0.0);
        const auto riderCameraAngleDegrees = [&viewModel]() {
            const double viewX = viewModel.cameraTargetX()
                    - viewModel.cameraX();
            const double viewZ = viewModel.cameraTargetZ()
                    - viewModel.cameraZ();
            const double riderX = viewModel.riderX()
                    - viewModel.cameraX();
            const double riderZ = viewModel.riderZ()
                    - viewModel.cameraZ();
            const double dot = viewX * riderX + viewZ * riderZ;
            const double lengths = std::hypot(viewX, viewZ)
                    * std::hypot(riderX, riderZ);
            return std::acos(std::clamp(dot / lengths, -1.0, 1.0))
                    * 180.0 / std::acos(-1.0);
        };
        QVERIFY2(riderCameraAngleDegrees() < 10.0,
                 qPrintable(QStringLiteral("main-line camera offset %1 degrees")
                     .arg(riderCameraAngleDegrees())));

        frame.feature.route = WorkoutGameRoute::SafeBypass;
        frame.feature.lateralOffsetMeters = 0.45;
        viewModel.setFrame(frame, 150.0, 220.0, 72, 145, 4);
        QVERIFY(std::abs(viewModel.riderRoll()) > 3.0);
        QVERIFY(std::abs(viewModel.riderRoll()) < std::abs(mainRoll));
        QVERIFY2(riderCameraAngleDegrees() < 10.0,
                 qPrintable(QStringLiteral("safe-line camera offset %1 degrees")
                     .arg(riderCameraAngleDegrees())));
    }

    void bermRollChangesByAtMostOneAndAHalfDegreesPerPresentedFrame()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Berm);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const WorkoutGameBermGeometryProfile profile =
                WorkoutGameBermGeometry::profile(piece->difficulty);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        constexpr double SpeedMetersPerSecond = 5.0;
        constexpr double FrameDistance = SpeedMetersPerSecond / 60.0;
        const double start = piece->challenge.obstacleDistanceMeters
                + profile.startMeters;
        const double end = piece->challenge.obstacleDistanceMeters
                + profile.endMeters;
        double priorRoll = 0.0;
        bool first = true;
        for (double distance = start; distance <= end;
             distance += FrameDistance) {
            WorkoutGameVisualSnapshot frame = frameAt(road, distance);
            frame.world.terrain = WorkoutGameTerrainKind::Berm;
            frame.simulation.speedKph = SpeedMetersPerSecond * 3.6;
            frame.feature.route = WorkoutGameRoute::MainLine;
            viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
            if (!first) {
                QVERIFY(std::abs(viewModel.riderRoll() - priorRoll)
                        <= 1.500001);
            }
            first = false;
            priorRoll = viewModel.riderRoll();
        }
    }

    void featureHudSeparatesPowerCadenceDistanceAndState()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Tabletop);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);

        WorkoutGameVisualSnapshot frame = frameAt(road, 12.0);
        frame.feature.ready = true;
        frame.feature.terrain = WorkoutGameTerrainKind::Tabletop;
        frame.feature.phase = WorkoutGameFeaturePhase::Measure;
        frame.feature.visualDistanceMeters = 12.0;
        frame.feature.decisionDistanceMeters = 17.5;
        frame.feature.actionStartDistanceMeters = 21.0;
        frame.simulation.challenge.enabled = true;
        frame.simulation.challenge.minimumEffortRatio = 1.0;
        frame.simulation.challenge.minimumCadenceRpm = 0.0;
        frame.simulation.challengeAssessment.effortReadiness = 0.75;
        frame.simulation.challengeAssessment.cadenceReadiness = 1.0;

        viewModel.setFrame(frame, 165.0, 220.0, 72, 148, 6);

        QVERIFY(viewModel.featureHudVisible());
        QCOMPARE(viewModel.featureName(), QStringLiteral("Tabletop"));
        QCOMPARE(viewModel.featureState(),
                 int(WorkoutGameFeatureHudState::Measure));
        QCOMPARE(viewModel.featureDistanceKind(),
                 int(WorkoutGameFeatureHudDistanceKind::Decision));
        QCOMPARE(viewModel.featureDistanceMeters(), 5.5);
        QCOMPARE(viewModel.requiredPowerWatts(), 220.0);
        QCOMPARE(viewModel.powerReadinessPercent(), 75);
        QVERIFY(!viewModel.cadenceRequired());
        QCOMPARE(viewModel.requiredCadenceRpm(), 0.0);
        QCOMPARE(viewModel.cadenceReadinessPercent(), 100);
    }

    void settingCourseClearsFeatureHudState()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Tabletop);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);

        WorkoutGameVisualSnapshot frame = frameAt(road, 12.0);
        frame.feature.ready = true;
        frame.feature.terrain = WorkoutGameTerrainKind::Tabletop;
        frame.feature.phase = WorkoutGameFeaturePhase::Measure;
        viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
        QVERIFY(viewModel.featureHudVisible());

        viewModel.setCourse(sampleCourse(), FtpWatts);

        QVERIFY(!viewModel.featureHudVisible());
        QVERIFY(viewModel.featureName().isEmpty());
        QVERIFY(viewModel.featureActionText().isEmpty());
        QVERIFY(viewModel.featureStatus().isEmpty());
        QCOMPARE(viewModel.readinessPercent(), 0);
    }

    void featureHudFitsSupportedWidths_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<double>("expectedHeight");
        QTest::newRow("narrow") << QSize(360, 640) << 166.0;
        QTest::newRow("desktop") << QSize(1280, 720) << 112.0;
        QTest::newRow("full-hd") << QSize(1920, 1080) << 112.0;
    }

    void featureHudFitsSupportedWidths()
    {
        QFETCH(QSize, size);
        QFETCH(double, expectedHeight);
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Tabletop);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        WorkoutGameVisualSnapshot frame = frameAt(road, 12.0);
        frame.feature.ready = true;
        frame.feature.terrain = WorkoutGameTerrainKind::Tabletop;
        frame.feature.phase = WorkoutGameFeaturePhase::Measure;
        frame.feature.visualDistanceMeters = 12.0;
        frame.feature.decisionDistanceMeters = 17.5;
        frame.simulation.challenge.enabled = true;
        frame.simulation.challenge.minimumEffortRatio = 1.0;
        frame.simulation.challengeAssessment.effortReadiness = 0.75;
        frame.simulation.challengeAssessment.cadenceReadiness = 1.0;
        viewModel.setFrame(frame, 165.0, 220.0, 72, 148, 6);

        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.resize(size);
        window.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        window.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        QCoreApplication::processEvents();

        auto *rootItem = qobject_cast<QQuickItem *>(window.rootObject());
        QVERIFY(rootItem);
        auto *hud = rootItem->findChild<QQuickItem *>(
                QStringLiteral("featureHud"));
        QVERIFY(hud);
        QVERIFY(hud->isVisible());
        QCOMPARE(hud->height(), expectedHeight);
        QVERIFY(hud->x() >= 0.0);
        QVERIFY(hud->y() >= 0.0);
        QVERIFY(hud->x() + hud->width() <= rootItem->width() + 1e-9);
        QVERIFY(hud->y() + hud->height() <= rootItem->height() + 1e-9);

        auto *stateLabel = rootItem->findChild<QQuickItem *>(
                QStringLiteral("featureStateLabel"));
        auto *powerBar = rootItem->findChild<QQuickItem *>(
                QStringLiteral("featurePowerBar"));
        auto *cadenceBar = rootItem->findChild<QQuickItem *>(
                QStringLiteral("featureCadenceBar"));
        QVERIFY(stateLabel && powerBar && cadenceBar);
        QVERIFY(stateLabel->width() > 0.0);
        QVERIFY(stateLabel->x() >= 0.0);
        QVERIFY(stateLabel->x() + stateLabel->width()
                <= stateLabel->parentItem()->width() + 1e-9);
        QVERIFY(powerBar->width() >= 0.0);
        QVERIFY(powerBar->width() <= powerBar->parentItem()->width() + 1e-9);
        QVERIFY(cadenceBar->width() >= 0.0);
        QVERIFY(cadenceBar->width()
                <= cadenceBar->parentItem()->width() + 1e-9);
    }

    void trainingHudPublishesProfileCursorGradeAndTelemetry()
    {
        const WorkoutGameCourse course = sampleCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);

        const QVariantList profile = viewModel.powerProfileSegments();
        QCOMPARE(profile.size(), int(course.sections.size()));
        QCOMPARE(profile.front().toMap().value(QStringLiteral("start")).toDouble(),
                 0.0);
        QCOMPARE(profile.back().toMap().value(QStringLiteral("end")).toDouble(),
                 1.0);
        QVERIFY(viewModel.powerProfileMaximumWatts() > 240.0);

        WorkoutGameVisualSnapshot frame = frameAt(road, 12.0);
        frame.simulation.workoutTimeMs = 25000;
        frame.world.gradePercent = 6.25;
        viewModel.setFrame(frame, 213.0, 219.0, 87, 151, 8);

        QCOMPARE(viewModel.workoutProgress(), 0.25);
        QCOMPARE(viewModel.gradePercent(), 6.25);
        QCOMPARE(viewModel.watts(), 213.0);
        QCOMPARE(viewModel.targetWatts(), 219.0);
        QCOMPARE(viewModel.cadenceRpm(), 87);
        QCOMPARE(viewModel.heartRate(), 151);
        QCOMPARE(viewModel.speedKph(), 22.5);
        QCOMPARE(viewModel.virtualGear(), 8);
        QCOMPARE(viewModel.workoutTimeSeconds(), 25);
        QCOMPARE(viewModel.generatorState(), QString());
        viewModel.setGeneratorState(QStringLiteral("On target"));
        QCOMPARE(viewModel.generatorState(), QStringLiteral("On target"));

        frame.simulation.workoutTimeMs = course.durationMs * 2;
        viewModel.setFrame(frame, 213.0, 219.0, 87, 151, 8);
        QCOMPARE(viewModel.workoutProgress(), 1.0);
        frame.simulation.workoutTimeMs = -1000;
        viewModel.setFrame(frame, 213.0, 219.0, 87, 151, 8);
        QCOMPARE(viewModel.workoutProgress(), 0.0);
    }

    void trainingHudFitsAndReportsMissingSensors_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("mobile-aspect") << QSize(360, 640);
        QTest::newRow("laptop") << QSize(1024, 600);
        QTest::newRow("full-hd") << QSize(1920, 1080);
    }

    void trainingHudFitsAndReportsMissingSensors()
    {
        QFETCH(QSize, size);
        const WorkoutGameCourse course = sampleCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        WorkoutGameVisualSnapshot frame = frameAt(road, 12.0);
        frame.simulation.workoutTimeMs = 25000;
        frame.world.gradePercent = 6.25;
        viewModel.setFrame(frame, 213.0, 219.0, 0, 0, 8);
        viewModel.setFps(58.75);

        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.resize(size);
        window.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        window.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        QCoreApplication::processEvents();

        auto *rootItem = qobject_cast<QQuickItem *>(window.rootObject());
        QVERIFY(rootItem);
        auto *hud = rootItem->findChild<QQuickItem *>(
                QStringLiteral("trainingHud"));
        auto *profile = rootItem->findChild<QQuickItem *>(
                QStringLiteral("powerProfile"));
        auto *cursor = rootItem->findChild<QQuickItem *>(
                QStringLiteral("powerProfileCursor"));
        QVERIFY(hud && profile && cursor);
        QVERIFY(hud->x() >= 0.0 && hud->y() >= 0.0);
        QVERIFY(hud->x() + hud->width() <= rootItem->width() + 1e-9);
        QVERIFY(hud->y() + hud->height() <= rootItem->height() + 1e-9);
        QVERIFY(cursor->x() >= 0.0);
        QVERIFY(cursor->x() + cursor->width() <= profile->width() + 1e-9);

        auto *cadence = findVisualItem(
                rootItem, QStringLiteral("cadenceValue"));
        auto *heartRate = findVisualItem(
                rootItem, QStringLiteral("heartRateValue"));
        auto *grade = findVisualItem(rootItem, QStringLiteral("gradeValue"));
        auto *fps = findVisualItem(rootItem, QStringLiteral("fpsValue"));
        auto *generatorBadge = rootItem->findChild<QQuickItem *>(
                QStringLiteral("generatorStateBadge"));
        QVERIFY(cadence && heartRate && grade && fps && generatorBadge);
        QVERIFY(!generatorBadge->isVisible());
        QCOMPARE(cadence->property("text").toString(), QStringLiteral("-- RPM"));
        QCOMPARE(heartRate->property("text").toString(), QStringLiteral("-- BPM"));
        QCOMPARE(grade->property("text").toString(), QStringLiteral("6.3%"));
        QCOMPARE(fps->property("text").toString(), QStringLiteral("58.8 FPS"));

        viewModel.setGeneratorState(QStringLiteral("Under target"));
        QCoreApplication::processEvents();
        QVERIFY(generatorBadge->isVisible());
        viewModel.setGeneratorState(QString());
        QCoreApplication::processEvents();
        QVERIFY(!generatorBadge->isVisible());

        if (hasInteractiveGraphicsPlatform()) {
            window.setFlag(Qt::BypassWindowManagerHint);
            window.resize(size);
            window.show();
            QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
            QTest::qWait(120);
            const QImage image = window.grabWindow();
            QVERIFY(!image.isNull());
            QCOMPARE(image.size(), size);
            QVERIFY2(sampledColorCount(image) > 40,
                     "training HUD layout appears blank");
            const QString outputDirectory = qEnvironmentVariable(
                    "GC_WORKOUT_GAME_3D_HUD_SCREENSHOT_DIR");
            if (!outputDirectory.isEmpty()) {
                QVERIFY(QDir().mkpath(outputDirectory));
                const QString filename = QDir(outputDirectory).filePath(
                        QStringLiteral("training-hud-%1x%2.png")
                            .arg(size.width()).arg(size.height()));
                QVERIFY2(image.save(filename), qPrintable(filename));
            }
        }

        QQuickItem *cadenceDelegate = cadence;
        viewModel.setTelemetry(225.0, 230.0, 91, 155, 9);
        QCoreApplication::processEvents();
        cadence = findVisualItem(rootItem, QStringLiteral("cadenceValue"));
        QCOMPARE(cadence, cadenceDelegate);
        QCOMPARE(cadence->property("text").toString(), QStringLiteral("91 RPM"));
    }

    void tabletopUsesOnlyTheProceduralRoadSurfaceInProduction()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Tabletop);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.terrain
                            == WorkoutGameTerrainKind::Tabletop
                            && candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(
                frameAt(road, 0.0), 220.0, 220.0, 88, 150, 7);
        const QVariantList features = viewModel.features();
        const auto asset = std::find_if(
                features.begin(), features.end(),
                [](const QVariant &entry) {
                    return entry.toMap().value(QStringLiteral("kind")).toInt()
                            == int(WorkoutGameTerrainKind::Tabletop);
                });
        QVERIFY(asset != features.end());
        const QVariantMap values = asset->toMap();
        QVERIFY(!values.contains(QStringLiteral("assetX")));
        QVERIFY(!values.contains(QStringLiteral("assetScaleY")));
        auto *trail = qobject_cast<WorkoutGame3DGeometry *>(
                viewModel.trailGeometry());
        auto *bypass = qobject_cast<WorkoutGame3DGeometry *>(
                viewModel.bypassGeometry());
        QVERIFY(trail && trail->ready());
        QVERIFY(bypass && bypass->ready());
    }

    void tabletopPosePreloadsFloatsAndAbsorbsLanding()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Tabletop);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const auto tabletop = WorkoutGameTabletopGeometry::profile(
                piece->difficulty);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);

        WorkoutGameVisualSnapshot preload = frameAt(
                road, piece->challenge.obstacleDistanceMeters
                    + tabletop.lipMeters - 0.45);
        preload.feature.ready = true;
        preload.feature.terrain = WorkoutGameTerrainKind::Tabletop;
        preload.feature.route = WorkoutGameRoute::MainLine;
        preload.world.terrain = WorkoutGameTerrainKind::Tabletop;
        for (int index = 0; index < 8; ++index) {
            preload.simulation.workoutTimeMs = 1000 + index * 80;
            viewModel.setFrame(preload, 250.0, 240.0, 88, 150, 7);
        }
        QVERIFY(viewModel.riderPump() < -0.03);

        WorkoutGameVisualSnapshot airborne = preload;
        airborne.world.rider.airborne = true;
        airborne.world.rider.clearanceMeters = 1.82;
        for (int index = 0; index < 8; ++index) {
            airborne.simulation.workoutTimeMs = 1800 + index * 80;
            viewModel.setFrame(airborne, 0.0, 240.0, 0, 150, 7);
        }
        QVERIFY(viewModel.riderPump() > 0.02);
        QCOMPARE(viewModel.riderAirHeight(), 1.0);

        WorkoutGameVisualSnapshot landed = preload;
        landed.world.landingImpact = 1.0;
        for (int index = 0; index < 8; ++index) {
            landed.simulation.workoutTimeMs = 2600 + index * 80;
            viewModel.setFrame(landed, 0.0, 240.0, 0, 150, 7);
        }
        QVERIFY(viewModel.riderPump() < -0.05);
        QCOMPARE(viewModel.landingImpact(), 1.0);
    }

    void packagedTabletopAssetLoadsWithRequiredNodes()
    {
        QQmlEngine engine;
        QQmlComponent component(
                &engine,
                QUrl(QStringLiteral(
                        "qrc:/qml/assets/Wg_Tabletop_Greybox.qml")));
        QStringList errors;
        for (const QQmlError &error : component.errors()) {
            errors.append(error.toString());
        }
        QVERIFY2(component.isReady(), qPrintable(errors.join('\n')));

        std::unique_ptr<QObject> asset(component.create());
        QVERIFY2(asset, qPrintable(errors.join('\n')));
        const std::array<const char *, 11> requiredObjects = {{
            "ROOT_Tabletop",
            "GEO_Tabletop_LOD0",
            "SOCKET_IN",
            "SOCKET_OUT",
            "MARKER_PREPARE",
            "MARKER_DECISION",
            "MARKER_ACTION",
            "MARKER_LIP",
            "MARKER_APEX",
            "MARKER_LAND",
            "MAT_TabletopBypass_Grey"
        }};
        for (const char *name : requiredObjects) {
            QVERIFY2(asset->findChild<QObject *>(
                    QString::fromLatin1(name)), name);
        }

        QFile mesh(QStringLiteral(
                ":/qml/assets/meshes/geo_Tabletop_LOD0_mesh.mesh"));
        QVERIFY(mesh.open(QIODevice::ReadOnly));
        QCOMPARE(mesh.size(), qint64(6976));
    }

    void logOverAssetUsesAuthoritativeRoadAnchorAndProfile()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::LogOver);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.terrain == WorkoutGameTerrainKind::LogOver
                            && candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    piece->terrain, piece->difficulty);
        QVERIFY(profile.ready);

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(frameAt(road, 0.0), 220.0, 220.0, 88, 150, 7);
        const QVariantList features = viewModel.features();
        const auto asset = std::find_if(
                features.begin(), features.end(),
                [](const QVariant &entry) {
                    return entry.toMap().value(QStringLiteral("kind")).toInt()
                            == int(WorkoutGameTerrainKind::LogOver);
                });
        QVERIFY(asset != features.end());
        const QVariantMap values = asset->toMap();
        const double expectedScale = profile.heightMeters / 0.54;
        QCOMPARE(values.value(QStringLiteral("assetScaleY")).toDouble(),
                 expectedScale);
        QCOMPARE(values.value(QStringLiteral("assetScaleZ")).toDouble(),
                 expectedScale);
        QVERIFY(values.contains(QStringLiteral("assetX")));
        QVERIFY(values.contains(QStringLiteral("assetY")));
        QVERIFY(values.contains(QStringLiteral("assetZ")));
    }

    void bunnyHopAssetUsesAuthoritativeRoadAnchorAndProfile()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::BunnyHop);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.terrain == WorkoutGameTerrainKind::BunnyHop
                            && candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    piece->terrain, piece->difficulty);
        QVERIFY(profile.ready);

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(frameAt(road, 0.0), 220.0, 220.0, 88, 150, 7);
        const QVariantList features = viewModel.features();
        const auto asset = std::find_if(
                features.begin(), features.end(),
                [](const QVariant &entry) {
                    return entry.toMap().value(QStringLiteral("kind")).toInt()
                            == int(WorkoutGameTerrainKind::BunnyHop);
                });
        QVERIFY(asset != features.end());
        const QVariantMap values = asset->toMap();
        QCOMPARE(values.value(QStringLiteral("assetScaleY")).toDouble(),
                 profile.heightMeters / 0.20);
        QCOMPARE(values.value(QStringLiteral("assetScaleZ")).toDouble(), 1.0);
        QVERIFY(values.contains(QStringLiteral("assetX")));
        QVERIFY(values.contains(QStringLiteral("assetY")));
        QVERIFY(values.contains(QStringLiteral("assetZ")));
    }

    void dropAssetUsesUpperSocketAnchorAndDepthScale()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Drop);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.terrain == WorkoutGameTerrainKind::Drop
                            && candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    piece->terrain, piece->difficulty);
        QVERIFY(profile.ready);

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(frameAt(road, 0.0), 220.0, 220.0, 88, 150, 7);
        const QVariantList features = viewModel.features();
        const auto asset = std::find_if(
                features.begin(), features.end(),
                [](const QVariant &entry) {
                    return entry.toMap().value(QStringLiteral("kind")).toInt()
                            == int(WorkoutGameTerrainKind::Drop);
                });
        QVERIFY(asset != features.end());
        const QVariantMap values = asset->toMap();
        QCOMPARE(values.value(QStringLiteral("assetScaleY")).toDouble(),
                 std::abs(profile.heightMeters) / 0.70);
        QCOMPARE(values.value(QStringLiteral("assetScaleZ")).toDouble(), 1.0);
        QVERIFY(values.contains(QStringLiteral("assetX")));
        QVERIFY(values.contains(QStringLiteral("assetY")));
        QVERIFY(values.contains(QStringLiteral("assetZ")));
    }

    void packagedBunnyHopAssetLoadsWithRequiredNodes()
    {
        QQmlEngine engine;
        QQmlComponent component(
                &engine,
                QUrl(QStringLiteral(
                        "qrc:/qml/assets/Wg_BunnyHop_Greybox.qml")));
        QStringList errors;
        for (const QQmlError &error : component.errors()) {
            errors.append(error.toString());
        }
        QVERIFY2(component.isReady(), qPrintable(errors.join('\n')));

        std::unique_ptr<QObject> asset(component.create());
        QVERIFY2(asset, qPrintable(errors.join('\n')));
        const std::array<const char *, 13> requiredObjects = {{
            "ROOT_BunnyHop",
            "GEO_BunnyHopHurdle_LOD0",
            "SOCKET_IN",
            "SOCKET_OUT",
            "MARKER_PREPARE",
            "MARKER_DECISION",
            "MARKER_ACTION",
            "MARKER_PRELOAD",
            "MARKER_TAKEOFF",
            "MARKER_APEX",
            "MARKER_LAND",
            "MAT_BunnyHopBar_Grey",
            "MAT_BunnyHopSupport_Grey"
        }};
        for (const char *name : requiredObjects) {
            QVERIFY2(asset->findChild<QObject *>(
                    QString::fromLatin1(name)), name);
        }
        QFile mesh(QStringLiteral(
                ":/qml/assets/meshes/geo_BunnyHopHurdle_LOD0_mesh.mesh"));
        QVERIFY(mesh.open(QIODevice::ReadOnly));
        QCOMPARE(mesh.size(), qint64(2164));
    }

    void packagedDropAssetLoadsWithRequiredNodes()
    {
        QQmlEngine engine;
        QQmlComponent component(
                &engine,
                QUrl(QStringLiteral(
                        "qrc:/qml/assets/Wg_Drop_Greybox.qml")));
        QStringList errors;
        for (const QQmlError &error : component.errors()) {
            errors.append(error.toString());
        }
        QVERIFY2(component.isReady(), qPrintable(errors.join('\n')));

        std::unique_ptr<QObject> asset(component.create());
        QVERIFY2(asset, qPrintable(errors.join('\n')));
        const std::array<const char *, 13> requiredObjects = {{
            "ROOT_Drop",
            "GEO_DropFace_LOD0",
            "SOCKET_IN",
            "SOCKET_OUT",
            "MARKER_PREPARE",
            "MARKER_DECISION",
            "MARKER_ACTION",
            "MARKER_LIP",
            "MARKER_AIR",
            "MARKER_LAND",
            "MARKER_RECOVERY",
            "MAT_DropFace_Grey",
            "MAT_DropEdge_Grey"
        }};
        for (const char *name : requiredObjects) {
            QVERIFY2(asset->findChild<QObject *>(
                    QString::fromLatin1(name)), name);
        }
        QFile mesh(QStringLiteral(
                ":/qml/assets/meshes/geo_DropFace_LOD0_mesh.mesh"));
        QVERIFY(mesh.open(QIODevice::ReadOnly));
        QCOMPARE(mesh.size(), qint64(2004));
    }

    void bypassRiderUsesTheSameBranchAndTerrainSurfaceAsItsMesh()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::LogOver);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const double distance =
                (piece->challenge.bypassStartDistanceMeters
                 + piece->challenge.bypassEndDistanceMeters) * 0.5;
        const double lateral = WorkoutGameTrailBranch::lateralAt(
                distance,
                piece->challenge.bypassStartDistanceMeters,
                piece->challenge.bypassEndDistanceMeters,
                piece->challenge.bypassLateralMeters);
        WorkoutGameVisualSnapshot frame = frameAt(road, distance);
        frame.feature.ready = true;
        frame.feature.route = WorkoutGameRoute::SafeBypass;
        frame.feature.outcome = WorkoutGameFeatureOutcome::Bypassed;
        frame.feature.lateralOffsetMeters = lateral;

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(frame, 150.0, 220.0, 72, 145, 4);

        const WorkoutGameRoadSample sample =
                WorkoutGameRoadCourseBuilder::sample(road, distance);
        const WorkoutGame3DTerrainProfileSnapshot terrain =
                WorkoutGame3DTerrainProfile::build(
                    sample, distance, road.seed);
        QVERIFY(sample.ready && terrain.ready);
        const double rightX = std::cos(sample.center.headingRadians);
        const double rightZ = -std::sin(sample.center.headingRadians);
        QVERIFY(std::abs(viewModel.riderX()
                - sample.center.xMeters - lateral * rightX) < 1e-9);
        QVERIFY(std::abs(viewModel.riderZ()
                - sample.center.zMeters - lateral * rightZ) < 1e-9);
        const double expectedY =
                WorkoutGame3DTerrainProfile::elevationAtLateral(
                    terrain, lateral)
                + WorkoutGameTrailBranch::treadLiftMeters(
                    WorkoutGameTrailBranch::blend(0.5));
        QVERIFY(std::abs(viewModel.riderY() - expectedY) < 1e-9);
    }

    void packagedLogOverAssetLoadsWithRequiredNodes()
    {
        QQmlEngine engine;
        QQmlComponent component(
                &engine,
                QUrl(QStringLiteral(
                        "qrc:/qml/assets/Wg_LogOver_Greybox.qml")));
        QStringList errors;
        for (const QQmlError &error : component.errors()) {
            errors.append(error.toString());
        }
        QVERIFY2(component.isReady(), qPrintable(errors.join('\n')));

        std::unique_ptr<QObject> asset(component.create());
        QVERIFY2(asset, qPrintable(errors.join('\n')));
        const std::array<const char *, 10> requiredObjects = {{
            "ROOT_LogOver",
            "GEO_LogOverObstacle_LOD0",
            "SOCKET_IN",
            "SOCKET_OUT",
            "MARKER_PREPARE",
            "MARKER_DECISION",
            "MARKER_ACTION",
            "MARKER_APEX",
            "MARKER_LAND",
            "MAT_LogOverBark_Grey"
        }};
        for (const char *name : requiredObjects) {
            QVERIFY2(asset->findChild<QObject *>(
                    QString::fromLatin1(name)), name);
        }
        QFile obstacle(QStringLiteral(
                ":/qml/assets/meshes/geo_LogOverObstacle_LOD0_mesh.mesh"));
        QVERIFY(obstacle.open(QIODevice::ReadOnly));
        QCOMPARE(obstacle.size(), qint64(3508));
    }

    void rendersPackagedLogOverAsset()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.setSource(QUrl(QStringLiteral(
                "qrc:/qml/assets/LogOverAssetHarness.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        window.resize(960, 540);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        QTest::qWait(500);

        const QImage rendered = window.grabWindow();
        QVERIFY(!rendered.isNull());
        QCOMPARE(rendered.size(), QSize(960, 540));
        QVERIFY2(sampledColorCount(rendered) > 6,
                 "packaged log-over mesh appears blank");
        const QString screenshot = qEnvironmentVariable(
                "GC_WORKOUT_GAME_LOG_OVER_ASSET_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QVERIFY2(rendered.save(screenshot), qPrintable(screenshot));
        }
    }

    void rendersPackagedBunnyHopAsset()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.setSource(QUrl(QStringLiteral(
                "qrc:/qml/assets/BunnyHopAssetHarness.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        window.resize(960, 540);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        QTest::qWait(500);

        const QImage rendered = window.grabWindow();
        QVERIFY(!rendered.isNull());
        QCOMPARE(rendered.size(), QSize(960, 540));
        QVERIFY2(sampledColorCount(rendered) > 7,
                 "packaged bunny-hop mesh appears blank");
        const QString screenshot = qEnvironmentVariable(
                "GC_WORKOUT_GAME_BUNNY_HOP_ASSET_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QVERIFY2(rendered.save(screenshot), qPrintable(screenshot));
        }
    }

    void rendersPackagedDropAsset()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.setSource(QUrl(QStringLiteral(
                "qrc:/qml/assets/DropAssetHarness.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        window.resize(960, 540);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        QTest::qWait(500);

        const QImage rendered = window.grabWindow();
        QVERIFY(!rendered.isNull());
        QCOMPARE(rendered.size(), QSize(960, 540));
        QVERIFY2(sampledColorCount(rendered) > 7,
                 "packaged drop mesh appears blank");
        const QString screenshot = qEnvironmentVariable(
                "GC_WORKOUT_GAME_DROP_ASSET_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QVERIFY2(rendered.save(screenshot), qPrintable(screenshot));
        }
    }

    void rendersPackagedTabletopAsset()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.setSource(QUrl(QStringLiteral(
                "qrc:/qml/assets/TabletopAssetHarness.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        window.resize(960, 540);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        QTest::qWait(500);

        const QImage rendered = window.grabWindow();
        QVERIFY(!rendered.isNull());
        QCOMPARE(rendered.size(), QSize(960, 540));
        QVERIFY2(sampledColorCount(rendered) > 12,
                 "packaged tabletop mesh appears blank");
        const QString screenshot = qEnvironmentVariable(
                "GC_WORKOUT_GAME_TABLETOP_ASSET_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QVERIFY2(rendered.save(screenshot), qPrintable(screenshot));
        }
    }

    void loadsRendersAndMovesScene()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const WorkoutGameCourse course = sampleCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        QVERIFY(viewModel.ready());
        auto *bypassGeometry = qobject_cast<WorkoutGame3DGeometry *>(
                viewModel.bypassGeometry());
        auto *bermGeometry = qobject_cast<WorkoutGame3DGeometry *>(
                viewModel.bermGeometry());
        auto *rootsGeometry = qobject_cast<WorkoutGame3DGeometry *>(
                viewModel.rootsGeometry());
        auto *rockGardenGeometry = qobject_cast<WorkoutGame3DGeometry *>(
                viewModel.rockGardenGeometry());
        auto *rockSlabGeometry = qobject_cast<WorkoutGame3DGeometry *>(
                viewModel.rockSlabGeometry());
        auto *forestDressingGeometry = qobject_cast<WorkoutGame3DGeometry *>(
                viewModel.forestDressingGeometry());
        QVERIFY(bypassGeometry);
        QVERIFY(bermGeometry);
        QVERIFY(rootsGeometry);
        QVERIFY(rockGardenGeometry);
        QVERIFY(rockSlabGeometry);
        QVERIFY(forestDressingGeometry);
        QVERIFY(bypassGeometry->ready());
        QVERIFY(bypassGeometry->sampleCount() >= 20);
        QVERIFY(forestDressingGeometry->ready());
        QVERIFY(forestDressingGeometry->sampleCount() >= 70);
        QVERIFY(viewModel.trees().size() <= 10);
        QVERIFY(viewModel.features().size() <= 32);
        viewModel.setFrame(
                frameAt(road, 12.0), 215.0, 220.0, 87, 148, 7);
        QCOMPARE(viewModel.workoutTimeSeconds(), 75);
        viewModel.setFps(59.7);

        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        window.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        window.resize(1280, 720);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        QTest::qWait(900);
        QVERIFY(!window.rootObject()->findChild<QObject *>(
                QStringLiteral("ROOT_Tabletop")));
        QVERIFY(window.rootObject()->findChild<QObject *>(
                QStringLiteral("riderGroundShadow")));
        QVERIFY(window.rootObject()->findChild<QObject *>(
                QStringLiteral("forestDressingModel")));

        const QImage first = window.grabWindow();
        QVERIFY(!first.isNull());
        QCOMPARE(first.size(), QSize(1280, 720));
        QVERIFY2(sampledColorCount(first) > 45,
                 "3D scene appears blank or nearly monochrome");

        QObject *firstFloorGeometry = viewModel.floorGeometry();
        viewModel.setFrame(
                frameAt(road, tabletopApproachDistance(road)),
                248.0, 242.0, 93, 154, 9);
        QTRY_VERIFY_WITH_TIMEOUT(
                viewModel.floorGeometry() != firstFloorGeometry, 3000);
        auto *floorGeometry = qobject_cast<WorkoutGame3DGeometry *>(
                viewModel.floorGeometry());
        QVERIFY(floorGeometry);
        QVERIFY(floorGeometry->ready());
        QTest::qWait(350);
        const QImage second = window.grabWindow();
        QVERIFY(!second.isNull());
        const int changed = changedPixels(first, second);
        QVERIFY2(changed > 900,
                 qPrintable(QStringLiteral("only %1 sampled pixels changed")
                         .arg(changed)));

        const QString screenshot = qEnvironmentVariable(
                "GC_WORKOUT_GAME_3D_SCREENSHOT",
                QDir(QDir::tempPath()).filePath(
                        QStringLiteral("workout-game-3d-test.png")));
        QVERIFY2(second.save(screenshot), qPrintable(screenshot));
    }

    void rendersSkinnyWithoutClearColorInsideTheTrail()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Skinny);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const double distance = piece->challenge.obstacleDistanceMeters;
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        WorkoutGameVisualSnapshot frame = frameAt(road, distance);
        frame.simulation.activeSection = 0;
        frame.simulation.sectionProgress = std::clamp(
                distance / road.totalLengthMeters, 0.0, 1.0);
        frame.simulation.route = WorkoutGameRoute::MainLine;
        frame.simulation.featureOutcome =
                WorkoutGameFeatureOutcome::Completed;
        frame.simulation.challenge = piece->challenge.profile;
        frame.feature = runtime.update(frame.simulation);

        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.setCourse(course, FtpWatts);
        window.resize(960, 540);
        window.setFrame(frame, 180.0, 175.0, 85, 148, 6);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        QTest::qWait(150);
        const QImage image = window.grabWindow();
        QVERIFY(!image.isNull());
        const QRect trailRegion(
                image.width() * 23 / 100,
                image.height() * 55 / 100,
                image.width() * 54 / 100,
                image.height() * 35 / 100);
        QVERIFY2(nearColorPixels(
                    image, QColor(QStringLiteral("#78a9bf")), trailRegion) < 10,
                 "Skinny trail contains a clear-color hole");
    }

    void productionWindowLoadsAndTearsDown()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const WorkoutGameCourse course = sampleCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);

        auto window = std::make_unique<WorkoutGame3DWindow>(true);
        QVERIFY(window->rendererAvailable());
        window->setCourse(course, FtpWatts);
        window->setFrame(
                frameAt(road, 18.0), 225.0, 220.0, 88, 149, 8);
        window->resize(960, 540);
        window->show();
        QTRY_VERIFY_WITH_TIMEOUT(window->isExposed(), 5000);
        QTest::qWait(300);
        const QImage image = window->grabWindow();
        QVERIFY(!image.isNull());
        QVERIFY(sampledColorCount(image) > 40);
        window->setSessionRunning(true);
        window->setFrame(
                frameAt(road, 24.0), 235.0, 230.0, 90, 151, 9);
        auto *rootItem = qobject_cast<QQuickItem *>(window->rootObject());
        QVERIFY(rootItem);
        QVERIFY(rootItem->findChild<QObject *>(
                    QStringLiteral("bypassGeometryModel")));
        QVERIFY(rootItem->findChild<QObject *>(
                    QStringLiteral("bermGeometryModel")));
        QVERIFY(rootItem->findChild<QObject *>(
                    QStringLiteral("rootsGeometryModel")));
        QVERIFY(rootItem->findChild<QObject *>(
                    QStringLiteral("rockGardenGeometryModel")));
        QVERIFY(rootItem->findChild<QObject *>(
                    QStringLiteral("rockSlabGeometryModel")));
        QVERIFY(rootItem->findChild<QObject *>(
                    QStringLiteral("skinnyGeometryModel")));
        QTRY_VERIFY_WITH_TIMEOUT(
                findVisualItem(rootItem, QStringLiteral("fpsValue"))
                    && findVisualItem(rootItem, QStringLiteral("fpsValue"))
                        ->property("text").toString()
                            != QStringLiteral("0.0 FPS"),
                2000);
        window->setSessionRunning(false);
        window.reset();
    }

    void exportsEveryFeatureAtTheLegacyViewpoint()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const QByteArray requestedOutput =
                qgetenv("GC_WORKOUT_GAME_3D_FEATURE_CATALOG_DIR");
        const QString outputDirectory = requestedOutput.isEmpty()
                ? QDir(QDir::tempPath()).filePath(
                    QStringLiteral("workout-game-3d-feature-catalog"))
                : QString::fromLocal8Bit(requestedOutput);
        QVERIFY(QDir().mkpath(outputDirectory));

        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.resize(1280, 720);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);

        for (const FeatureCatalogEntry &entry : FeatureCatalog) {
            const WorkoutGameCourse course = catalogCourse(entry.terrain);
            const WorkoutGameRoadCourse road =
                    WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
            QVERIFY(road.ready);
            const auto challenge = std::find_if(
                    road.pieces.begin(), road.pieces.end(),
                    [](const WorkoutGameRoadPiece &piece) {
                        return piece.challenge.enabled;
                    });
            QVERIFY(challenge != road.pieces.end());
            const WorkoutGameRoadTimelineSection &timeline =
                    road.timeline.front();
            const double distance = std::max(
                    timeline.startDistanceMeters,
                    challenge->challenge.obstacleDistanceMeters - 10.0);
            const double progress = std::clamp(
                    (distance - timeline.startDistanceMeters)
                        / (timeline.endDistanceMeters
                           - timeline.startDistanceMeters),
                    0.0, 1.0);
            WorkoutGameSimulationSnapshot simulation;
            simulation.ready = true;
            simulation.activeSection = 0;
            simulation.sectionProgress = progress;
            simulation.workoutTimeMs = std::int64_t(std::llround(
                    course.durationMs * progress));
            simulation.courseProgress = progress;
            simulation.speedKph = 20.0;
            simulation.featureOutcome = WorkoutGameFeatureOutcome::Completed;
            simulation.route = WorkoutGameRoute::MainLine;
            simulation.challengeReadiness = 1.0;
            simulation.challenge = challenge->challenge.profile;
            WorkoutGameFeatureChallengeMetrics metrics;
            metrics.averageActualWatts = 220.0;
            metrics.averageTargetWatts = 220.0;
            metrics.averageEffortRatio = 1.0;
            metrics.averageCadenceRpm = 88.0;
            metrics.averageSpeedKph = 20.0;
            metrics.averageAdherence = 1.0;
            simulation.challengeAssessment =
                    WorkoutGameFeatureChallenge::assess(
                        simulation.challenge, metrics);
            WorkoutGameFeatureRuntime runtime;
            QVERIFY(runtime.configure(road));
            WorkoutGameVisualSnapshot frame;
            frame.simulation = simulation;
            frame.feature = runtime.update(simulation);
            frame.world.ready = true;
            frame.world.generation = 1;
            frame.world.terrain = entry.terrain;
            frame.world.gradePercent = course.sections.front().gradePercent;
            frame.world.rider.distanceMeters = distance;
            frame.world.rider.clearanceMeters = 0.82;
            frame.world.speedMetersPerSecond = 20.0 / 3.6;

            window.setCourse(course, FtpWatts);
            window.setFrame(frame, 220.0, 220.0, 88, 150, 7);
            QTest::qWait(350);
            const QImage rendered = window.grabWindow();
            QVERIFY(!rendered.isNull());
            QCOMPARE(rendered.size(), QSize(1280, 720));
            QVERIFY2(sampledColorCount(rendered) > 35,
                     "3D feature scene appears blank or nearly monochrome");
            const QString output = QDir(outputDirectory).filePath(
                    QStringLiteral("feature-%1.png")
                        .arg(QString::fromLatin1(entry.name)));
            QVERIFY2(rendered.save(output), qPrintable(output));
        }
    }

    void rendersAuthoredChallengeCompletedAndBypassedLines_data()
    {
        QTest::addColumn<int>("terrainValue");
        QTest::addColumn<QByteArray>("screenshotEnvironment");
        QTest::addColumn<QString>("featureName");
        QTest::addColumn<double>("mainClearanceMeters");
        QTest::newRow("log-over")
                << int(WorkoutGameTerrainKind::LogOver)
                << QByteArray("GC_WORKOUT_GAME_LOG_OVER_LINE_SCREENSHOT_DIR")
                << QStringLiteral("log-over") << 1.12;
        QTest::newRow("bunny-hop")
                << int(WorkoutGameTerrainKind::BunnyHop)
                << QByteArray("GC_WORKOUT_GAME_BUNNY_HOP_LINE_SCREENSHOT_DIR")
                << QStringLiteral("bunny-hop") << 1.02;
        QTest::newRow("tabletop")
                << int(WorkoutGameTerrainKind::Tabletop)
                << QByteArray("GC_WORKOUT_GAME_TABLETOP_LINE_SCREENSHOT_DIR")
                << QStringLiteral("tabletop") << 1.32;
        QTest::newRow("drop")
                << int(WorkoutGameTerrainKind::Drop)
                << QByteArray("GC_WORKOUT_GAME_DROP_LINE_SCREENSHOT_DIR")
                << QStringLiteral("drop") << 0.82;
    }

    void rendersAuthoredChallengeCompletedAndBypassedLines()
    {
        QFETCH(int, terrainValue);
        QFETCH(QByteArray, screenshotEnvironment);
        QFETCH(QString, featureName);
        QFETCH(double, mainClearanceMeters);
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const WorkoutGameTerrainKind terrain =
                WorkoutGameTerrainKind(terrainValue);
        const WorkoutGameCourse course = catalogCourse(
                terrain);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const double distance = piece->challenge.obstacleDistanceMeters
                - (terrain == WorkoutGameTerrainKind::Drop ? 1.5 : 0.0);

        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.resize(1280, 720);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        window.setCourse(course, FtpWatts);

        WorkoutGameVisualSnapshot main = frameAt(road, distance);
        main.simulation.challenge = piece->challenge.profile;
        WorkoutGameFeatureChallengeMetrics mainMetrics;
        mainMetrics.averageActualWatts = 235.0;
        mainMetrics.averageTargetWatts = 220.0;
        mainMetrics.averageEffortRatio = 235.0 / 220.0;
        mainMetrics.averageCadenceRpm = 92.0;
        mainMetrics.averageSpeedKph = 22.5;
        mainMetrics.averageAdherence = 1.0;
        main.simulation.challengeAssessment =
                WorkoutGameFeatureChallenge::assess(
                    main.simulation.challenge, mainMetrics);
        main.feature.ready = true;
        main.feature.terrain = terrain;
        main.feature.route = WorkoutGameRoute::MainLine;
        main.feature.outcome = WorkoutGameFeatureOutcome::Completed;
        main.feature.phase = WorkoutGameFeaturePhase::Recovery;
        main.world.rider.clearanceMeters = mainClearanceMeters;
        main.world.rider.airborne =
                terrain != WorkoutGameTerrainKind::Drop;
        window.setFrame(main, 235.0, 220.0, 92, 152, 7);
        QTest::qWait(350);
        const QImage completed = window.grabWindow();
        QVERIFY(!completed.isNull());
        QVERIFY(sampledColorCount(completed) > 35);

        WorkoutGameVisualSnapshot bypassed = frameAt(road, distance);
        bypassed.simulation.challenge = piece->challenge.profile;
        WorkoutGameFeatureChallengeMetrics bypassMetrics;
        bypassMetrics.averageActualWatts = 150.0;
        bypassMetrics.averageTargetWatts = 220.0;
        bypassMetrics.averageEffortRatio = 150.0 / 220.0;
        bypassMetrics.averageCadenceRpm = 72.0;
        bypassMetrics.averageSpeedKph = 22.5;
        bypassMetrics.averageAdherence = 150.0 / 220.0;
        bypassed.simulation.challengeAssessment =
                WorkoutGameFeatureChallenge::assess(
                    bypassed.simulation.challenge, bypassMetrics);
        bypassed.feature.ready = true;
        bypassed.feature.terrain = terrain;
        bypassed.feature.route = WorkoutGameRoute::SafeBypass;
        bypassed.feature.outcome = WorkoutGameFeatureOutcome::Bypassed;
        bypassed.feature.phase = WorkoutGameFeaturePhase::Recovery;
        bypassed.feature.lateralOffsetMeters =
                WorkoutGameTrailBranch::lateralAt(
                    distance,
                    piece->challenge.bypassStartDistanceMeters,
                    piece->challenge.bypassEndDistanceMeters,
                    piece->challenge.bypassLateralMeters);
        QVERIFY(std::abs(bypassed.feature.lateralOffsetMeters) >= 1.67);
        bypassed.world.rider.clearanceMeters = 0.82;
        window.setCourse(course, FtpWatts);
        window.setFrame(bypassed, 150.0, 220.0, 72, 145, 4);
        QTest::qWait(350);
        const QImage bypass = window.grabWindow();
        QVERIFY(!bypass.isNull());
        QVERIFY(sampledColorCount(bypass) > 35);
        QVERIFY2(changedPixels(completed, bypass) > 120,
                 qPrintable(featureName
                     + QStringLiteral(" completed and bypassed lines look identical")));

        auto *rootItem = qobject_cast<QQuickItem *>(window.rootObject());
        QVERIFY(rootItem);
        QObject *bypassModel = rootItem->findChild<QObject *>(
                QStringLiteral("bypassGeometryModel"));
        QVERIFY(bypassModel);
        auto *renderedBypass = qobject_cast<WorkoutGame3DGeometry *>(
                bypassModel->property("geometry").value<QObject *>());
        QVERIFY(renderedBypass);
        QVERIFY(renderedBypass->ready());
        QVERIFY(bypassModel->setProperty("visible", false));
        QTest::qWait(200);
        const QImage withoutBypass = window.grabWindow();
        QVERIFY(!withoutBypass.isNull());
        const int visibleBypassPixels = changedPixels(bypass, withoutBypass);
        QVERIFY(bypassModel->setProperty("visible", true));

        const QString outputDirectory = qEnvironmentVariable(
                screenshotEnvironment.constData());
        if (!outputDirectory.isEmpty()) {
            QVERIFY(QDir().mkpath(outputDirectory));
            QVERIFY(completed.save(QDir(outputDirectory).filePath(
                    QStringLiteral("completed.png"))));
            QVERIFY(bypass.save(QDir(outputDirectory).filePath(
                    QStringLiteral("bypassed.png"))));
            QVERIFY(withoutBypass.save(QDir(outputDirectory).filePath(
                    QStringLiteral("without-bypass.png"))));
        }
        QVERIFY2(visibleBypassPixels > 300,
                 qPrintable(QStringLiteral(
                     "bypass changed only %1 rendered pixels")
                     .arg(visibleBypassPixels)));
    }

    void nonRunningFramesDoNotRemainInStaleSmootherState()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::LogOver);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);

        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.resize(960, 540);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        window.setCourse(course, FtpWatts);
        window.setFrame(frameAt(road, 20.0), 180.0, 220.0, 80, 145, 5);
        window.setFrame(frameAt(road, 60.0), 190.0, 220.0, 82, 146, 6);

        auto *rootItem = qobject_cast<QQuickItem *>(window.rootObject());
        QVERIFY(rootItem);
        QObject *riderNode = rootItem->findChild<QObject *>(
                QStringLiteral("riderNode"));
        QVERIFY(riderNode);
        const WorkoutGameRoadSample expected =
                WorkoutGameRoadCourseBuilder::sample(road, 60.0);
        QVERIFY(expected.ready);
        const QVector3D position =
                riderNode->property("position").value<QVector3D>();
        QVERIFY(std::abs(position.x() - expected.center.xMeters) < 0.001);
        QVERIFY(std::abs(position.z() - expected.center.zMeters) < 0.001);
    }

    void exportsAuthoredChallengeCompletedAndBypassedMotionFrames_data()
    {
        QTest::addColumn<int>("terrainValue");
        QTest::addColumn<QByteArray>("videoEnvironment");
        QTest::addColumn<double>("maximumExpectedAirMeters");
        QTest::newRow("log-over")
                << int(WorkoutGameTerrainKind::LogOver)
                << QByteArray("GC_WORKOUT_GAME_LOG_OVER_VIDEO_DIR") << 0.73;
        QTest::newRow("bunny-hop")
                << int(WorkoutGameTerrainKind::BunnyHop)
                << QByteArray("GC_WORKOUT_GAME_BUNNY_HOP_VIDEO_DIR") << 0.43;
        QTest::newRow("tabletop")
                << int(WorkoutGameTerrainKind::Tabletop)
                << QByteArray("GC_WORKOUT_GAME_TABLETOP_VIDEO_DIR") << 1.80;
        QTest::newRow("drop")
                << int(WorkoutGameTerrainKind::Drop)
                << QByteArray("GC_WORKOUT_GAME_DROP_VIDEO_DIR") << 1.10;
    }

    void exportsGroundedRollerPumpMotionFrames()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const QString outputDirectory = qEnvironmentVariable(
                "GC_WORKOUT_GAME_ROLLERS_VIDEO_DIR");
        if (outputDirectory.isEmpty()) {
            QSKIP("Set GC_WORKOUT_GAME_ROLLERS_VIDEO_DIR to export frames");
        }
        QVERIFY(QDir().mkpath(outputDirectory));

        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Rollers);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const WorkoutGameFeatureGeometryProfile profile =
                WorkoutGameFeatureGeometry::profile(
                    WorkoutGameTerrainKind::Rollers, piece->difficulty);
        QVERIFY(profile.ready);

        WorkoutGameFeatureRuntime runtime;
        WorkoutGamePhysics physics;
        QVERIFY(runtime.configure(road));
        QVERIFY(physics.configure(road));
        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.setCourse(course, FtpWatts);
        window.resize(960, 540);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);

        constexpr int FrameCount = 72;
        constexpr double SpeedMetersPerSecond = 7.0;
        const double start = piece->challenge.obstacleDistanceMeters
                + profile.startMeters;
        const double end = piece->challenge.obstacleDistanceMeters
                + profile.endMeters;
        int airborneFrames = 0;
        int visiblyChangedFrames = 0;
        double minimumBodyY = 10.0;
        double maximumBodyY = -10.0;
        double maximumPitch = 0.0;
        QImage previous;
        for (int frameIndex = 0; frameIndex < FrameCount; ++frameIndex) {
            const double progress = double(frameIndex)
                    / double(FrameCount - 1);
            const double distance = start + (end - start) * progress;
            WorkoutGameVisualSnapshot frame = frameAt(road, distance);
            frame.simulation.activeSection = 0;
            frame.simulation.sectionProgress = std::clamp(
                    distance / road.totalLengthMeters, 0.0, 1.0);
            frame.simulation.route = WorkoutGameRoute::MainLine;
            frame.simulation.featureOutcome =
                    WorkoutGameFeatureOutcome::Completed;
            frame.simulation.speedKph = SpeedMetersPerSecond * 3.6;
            frame.feature = runtime.update(frame.simulation);
            WorkoutGamePhysicsInput input;
            input.workoutTimeMs = std::int64_t(std::llround(
                    (distance - start) / SpeedMetersPerSecond * 1000.0));
            input.courseDistanceMeters = distance;
            input.terrain = WorkoutGameTerrainKind::Rollers;
            input.desiredSpeedMetersPerSecond = SpeedMetersPerSecond;
            input.effortRatio = 1.0;
            frame.world = physics.update(input);
            QVERIFY(frame.world.ready);
            if (frame.world.rider.airborne) ++airborneFrames;
            maximumPitch = std::max(
                    maximumPitch, std::abs(frame.world.rider.pitchDegrees));

            window.setFrame(frame, 220.0, 220.0, 88, 150, 7);
            QTest::qWait(12);
            QObject *body = window.rootObject()->findChild<QObject *>(
                    QStringLiteral("riderBodyNode"));
            QVERIFY(body);
            const double bodyY = body->property("y").toDouble();
            minimumBodyY = std::min(minimumBodyY, bodyY);
            maximumBodyY = std::max(maximumBodyY, bodyY);
            const QImage image = window.grabWindow();
            QVERIFY(!image.isNull());
            QVERIFY(sampledColorCount(image) > 30);
            if (!previous.isNull() && changedPixels(previous, image) > 20) {
                ++visiblyChangedFrames;
            }
            previous = image;
            const QString path = QDir(outputDirectory).filePath(
                    QStringLiteral("frame-%1.png")
                        .arg(frameIndex, 4, 10, QLatin1Char('0')));
            QVERIFY(image.save(path));
        }

        QCOMPARE(airborneFrames, 0);
        QVERIFY(maximumPitch <= 18.0);
        QVERIFY2(maximumBodyY - minimumBodyY >= 0.05,
                 qPrintable(QStringLiteral("roller body range was %1 m")
                     .arg(maximumBodyY - minimumBodyY)));
        QVERIFY(visiblyChangedFrames > FrameCount * 4 / 5);
    }

    void exportsBankedBermMainAndSafeLineMotionFrames()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const QString outputRoot = qEnvironmentVariable(
                "GC_WORKOUT_GAME_BERM_VIDEO_DIR");
        if (outputRoot.isEmpty()) {
            QSKIP("Set GC_WORKOUT_GAME_BERM_VIDEO_DIR to export frames");
        }
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Berm);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const WorkoutGameBermGeometryProfile profile =
                WorkoutGameBermGeometry::profile(piece->difficulty);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));

        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.resize(960, 540);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        auto *bermViewModel = qobject_cast<WorkoutGame3DViewModel *>(
                window.rootContext()->contextProperty(
                    QStringLiteral("workoutGame3D")).value<QObject *>());
        QVERIFY(bermViewModel);
        constexpr int FrameCount = 96;
        const double start = piece->challenge.obstacleDistanceMeters
                + profile.startMeters - 2.0;
        const double end = piece->challenge.obstacleDistanceMeters
                + profile.endMeters + 2.0;

        for (WorkoutGameRoute route : {
                 WorkoutGameRoute::MainLine,
                 WorkoutGameRoute::SafeBypass}) {
            window.setCourse(course, FtpWatts);
            const bool safe = route == WorkoutGameRoute::SafeBypass;
            const QString directory = QDir(outputRoot).filePath(
                    safe ? QStringLiteral("safe-line")
                         : QStringLiteral("main-line"));
            QVERIFY(QDir().mkpath(directory));
            double maximumLateral = 0.0;
            double maximumRoll = 0.0;
            double previousLateral = 0.0;
            for (int frameIndex = 0; frameIndex < FrameCount; ++frameIndex) {
                const double progress = double(frameIndex)
                        / double(FrameCount - 1);
                const double distance = start + (end - start) * progress;
                WorkoutGameVisualSnapshot frame = frameAt(road, distance);
                frame.simulation.activeSection = 0;
                frame.simulation.sectionProgress = std::clamp(
                        distance / road.totalLengthMeters, 0.0, 1.0);
                frame.simulation.route = route;
                frame.simulation.featureOutcome = safe
                        ? WorkoutGameFeatureOutcome::Bypassed
                        : WorkoutGameFeatureOutcome::Completed;
                frame.simulation.speedKph = 20.0;
                frame.feature = runtime.update(frame.simulation);
                frame.world.terrain = WorkoutGameTerrainKind::Berm;
                frame.world.rider.airborne = false;
                window.setFrame(frame,
                        safe ? 150.0 : 225.0, 220.0,
                        safe ? 72 : 88, 150, safe ? 4 : 7);
                QTest::qWait(12);
                maximumLateral = std::max(
                        maximumLateral,
                        std::abs(frame.feature.lateralOffsetMeters));
                if (frameIndex > 0) {
                    QVERIFY(std::abs(frame.feature.lateralOffsetMeters
                                - previousLateral) < 0.04);
                }
                previousLateral = frame.feature.lateralOffsetMeters;
                maximumRoll = std::max(
                        maximumRoll, std::abs(bermViewModel->riderRoll()));
                const QImage image = window.grabWindow();
                QVERIFY(!image.isNull());
                QVERIFY(sampledColorCount(image) > 30);
                const QString path = QDir(directory).filePath(
                        QStringLiteral("frame-%1.png")
                            .arg(frameIndex, 4, 10, QLatin1Char('0')));
                QImageWriter writer(path, "png");
                writer.setCompression(1);
                QVERIFY2(writer.write(image), qPrintable(writer.errorString()));
            }
            if (safe) {
                QVERIFY(maximumLateral > 0.40);
                QVERIFY(maximumRoll > 4.0);
                QVERIFY2(maximumRoll < 12.0,
                         qPrintable(QStringLiteral(
                             "safe-line roll reached %1 degrees")
                             .arg(maximumRoll)));
            } else {
                QCOMPARE(maximumLateral, 0.0);
                QVERIFY(maximumRoll > 20.0);
                QVERIFY(maximumRoll <= 28.0);
            }
        }
    }

    void exportsRootsMainAndSafeLineMotionFrames()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const QString outputRoot = qEnvironmentVariable(
                "GC_WORKOUT_GAME_ROOTS_VIDEO_DIR");
        if (outputRoot.isEmpty()) {
            QSKIP("Set GC_WORKOUT_GAME_ROOTS_VIDEO_DIR to export frames");
        }
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Roots);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const WorkoutGameRootGeometryProfile roots =
                WorkoutGameRootGeometry::profile(piece->difficulty);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));

        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.resize(960, 540);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        constexpr int FrameCount = 192;
        constexpr double SpeedMetersPerSecond = 5.0;
        const double start = piece->challenge.obstacleDistanceMeters
                + roots.startMeters - 2.0;
        const double end = piece->challenge.obstacleDistanceMeters
                + roots.endMeters + 2.0;
        double mainSuspensionRange = 0.0;
        double safeSuspensionRange = 0.0;

        for (WorkoutGameRoute route : {
                 WorkoutGameRoute::MainLine,
                 WorkoutGameRoute::SafeBypass}) {
            const bool safe = route == WorkoutGameRoute::SafeBypass;
            const QString directory = QDir(outputRoot).filePath(
                    safe ? QStringLiteral("safe-line")
                         : QStringLiteral("main-line"));
            QVERIFY(QDir().mkpath(directory));
            window.setCourse(course, FtpWatts);
            WorkoutGamePhysics physics;
            QVERIFY(physics.configure(road));
            double minimumSuspension = 1.0;
            double maximumSuspension = 0.0;
            double maximumLateral = 0.0;
            double previousLateral = 0.0;
            int visiblyChangedFrames = 0;
            int airborneFrames = 0;
            QImage previous;
            for (int frameIndex = 0; frameIndex < FrameCount; ++frameIndex) {
                const double progress = double(frameIndex)
                        / double(FrameCount - 1);
                const double distance = start + (end - start) * progress;
                WorkoutGameVisualSnapshot frame = frameAt(road, distance);
                frame.simulation.activeSection = 0;
                frame.simulation.sectionProgress = std::clamp(
                        distance / road.totalLengthMeters, 0.0, 1.0);
                frame.simulation.workoutTimeMs = std::int64_t(std::llround(
                        (distance - start) / SpeedMetersPerSecond * 1000.0));
                frame.simulation.route = route;
                frame.simulation.featureOutcome = safe
                        ? WorkoutGameFeatureOutcome::Bypassed
                        : WorkoutGameFeatureOutcome::Completed;
                frame.simulation.challenge = piece->challenge.profile;
                WorkoutGameFeatureChallengeMetrics metrics;
                metrics.averageActualWatts = safe ? 165.0 : 225.0;
                metrics.averageTargetWatts = 220.0;
                metrics.averageEffortRatio = safe ? 0.75 : 225.0 / 220.0;
                metrics.averageCadenceRpm = safe ? 78.0 : 88.0;
                metrics.averageSpeedKph = SpeedMetersPerSecond * 3.6;
                metrics.averageAdherence = safe ? 0.75 : 1.0;
                frame.simulation.challengeAssessment =
                        WorkoutGameFeatureChallenge::assess(
                            frame.simulation.challenge, metrics);
                frame.simulation.speedKph = SpeedMetersPerSecond * 3.6;
                frame.feature = runtime.update(frame.simulation);
                WorkoutGamePhysicsInput input;
                input.workoutTimeMs = frame.simulation.workoutTimeMs;
                input.courseDistanceMeters = distance;
                input.terrain = WorkoutGameTerrainKind::Roots;
                input.desiredSpeedMetersPerSecond = SpeedMetersPerSecond;
                input.effortRatio = safe ? 0.75 : 1.0;
                input.forceGroundFollowing = safe;
                frame.world = physics.update(input);
                QVERIFY(frame.world.ready);
                if (frame.world.rider.airborne) ++airborneFrames;
                const double suspension = 0.5 * (
                        frame.world.rider.rearSuspension
                        + frame.world.rider.frontSuspension);
                if (distance >= piece->challenge.obstacleDistanceMeters
                                + roots.activeStartMeters
                        && distance <= piece->challenge.obstacleDistanceMeters
                                + roots.activeEndMeters) {
                    minimumSuspension = std::min(
                            minimumSuspension, suspension);
                    maximumSuspension = std::max(
                            maximumSuspension, suspension);
                }
                maximumLateral = std::max(maximumLateral,
                        std::abs(frame.feature.lateralOffsetMeters));
                if (frameIndex > 0) {
                    const double lateralStep = std::abs(
                            frame.feature.lateralOffsetMeters
                                - previousLateral);
                    QVERIFY2(lateralStep < 0.05,
                             qPrintable(QStringLiteral(
                                 "lateral step %1 at frame %2: %3 -> %4")
                                 .arg(lateralStep)
                                 .arg(frameIndex)
                                 .arg(previousLateral)
                                 .arg(frame.feature.lateralOffsetMeters)));
                }
                previousLateral = frame.feature.lateralOffsetMeters;

                window.setFrame(frame,
                        safe ? 165.0 : 225.0, 220.0,
                        safe ? 78 : 88, 150, safe ? 4 : 7);
                QTest::qWait(8);
                if (frame.feature.phase
                            == WorkoutGameFeaturePhase::Action) {
                    auto *rootItem = qobject_cast<QQuickItem *>(
                            window.rootObject());
                    auto *powerValue = rootItem
                            ? findVisualItem(
                                rootItem,
                                QStringLiteral("featurePowerValue"))
                            : nullptr;
                    QVERIFY(powerValue);
                    QVERIFY2(powerValue->property("text").toString()
                                    .contains(QStringLiteral("/ 220 W")),
                             qPrintable(powerValue->property("text")
                                    .toString()));
                }
                const QImage image = window.grabWindow();
                QVERIFY(!image.isNull());
                QVERIFY(sampledColorCount(image) > 30);
                if (!previous.isNull()
                        && changedPixels(previous, image) > 20) {
                    ++visiblyChangedFrames;
                }
                previous = image;
                const QString path = QDir(directory).filePath(
                        QStringLiteral("frame-%1.png")
                            .arg(frameIndex, 4, 10, QLatin1Char('0')));
                QImageWriter writer(path, "png");
                writer.setCompression(1);
                QVERIFY2(writer.write(image), qPrintable(writer.errorString()));
            }
            QCOMPARE(airborneFrames, 0);
            QVERIFY(visiblyChangedFrames > FrameCount * 4 / 5);
            if (safe) {
                QCOMPARE(maximumLateral, roots.safeLineLateralMeters);
                safeSuspensionRange = maximumSuspension - minimumSuspension;
            } else {
                QCOMPARE(maximumLateral, 0.0);
                mainSuspensionRange = maximumSuspension - minimumSuspension;
            }
        }
        QVERIFY(mainSuspensionRange > 0.03);
        QVERIFY2(safeSuspensionRange <= mainSuspensionRange * 0.25,
                 qPrintable(QStringLiteral(
                     "safe suspension range %1 exceeded 25 percent of main %2")
                     .arg(safeSuspensionRange).arg(mainSuspensionRange)));
    }

    void exportsRockGardenMainAndSafeLineMotionFrames()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const QString outputRoot = qEnvironmentVariable(
                "GC_WORKOUT_GAME_ROCK_GARDEN_VIDEO_DIR");
        if (outputRoot.isEmpty()) {
            QSKIP("Set GC_WORKOUT_GAME_ROCK_GARDEN_VIDEO_DIR to export frames");
        }
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::RockGarden);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const WorkoutGameRockGardenGeometryProfile rocks =
                WorkoutGameRockGardenGeometry::profile(piece->difficulty);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));

        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.resize(960, 540);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        constexpr int FrameCount = 288;
        constexpr double SpeedMetersPerSecond = 5.0;
        const double start = piece->challenge.obstacleDistanceMeters
                + rocks.startMeters - 5.0;
        const double end = piece->challenge.obstacleDistanceMeters
                + rocks.endMeters + 5.0;
        double mainSuspensionRange = 0.0;
        double safeSuspensionRange = 0.0;

        for (WorkoutGameRoute route : {
                 WorkoutGameRoute::MainLine,
                 WorkoutGameRoute::SafeBypass}) {
            const bool safe = route == WorkoutGameRoute::SafeBypass;
            const QString directory = QDir(outputRoot).filePath(
                    safe ? QStringLiteral("safe-line")
                         : QStringLiteral("main-line"));
            QVERIFY(QDir().mkpath(directory));
            window.setCourse(course, FtpWatts);
            WorkoutGamePhysics physics;
            QVERIFY(physics.configure(road));
            double minimumSuspension = 1.0;
            double maximumSuspension = 0.0;
            double maximumLateral = 0.0;
            double previousLateral = 0.0;
            int visiblyChangedFrames = 0;
            QImage previous;
            for (int frameIndex = 0; frameIndex < FrameCount; ++frameIndex) {
                const double progress = double(frameIndex)
                        / double(FrameCount - 1);
                const double distance = start + (end - start) * progress;
                WorkoutGameVisualSnapshot frame = frameAt(road, distance);
                frame.simulation.activeSection = 0;
                frame.simulation.sectionProgress = std::clamp(
                        distance / road.totalLengthMeters, 0.0, 1.0);
                frame.simulation.workoutTimeMs = std::int64_t(std::llround(
                        (distance - start) / SpeedMetersPerSecond * 1000.0));
                frame.simulation.route = route;
                frame.simulation.featureOutcome = safe
                        ? WorkoutGameFeatureOutcome::Bypassed
                        : WorkoutGameFeatureOutcome::Completed;
                frame.simulation.challenge = piece->challenge.profile;
                WorkoutGameFeatureChallengeMetrics metrics;
                metrics.averageActualWatts = safe ? 165.0 : 225.0;
                metrics.averageTargetWatts = 220.0;
                metrics.averageEffortRatio = safe ? 0.75 : 225.0 / 220.0;
                metrics.averageCadenceRpm = safe ? 78.0 : 88.0;
                metrics.averageSpeedKph = SpeedMetersPerSecond * 3.6;
                metrics.averageAdherence = safe ? 0.75 : 1.0;
                frame.simulation.challengeAssessment =
                        WorkoutGameFeatureChallenge::assess(
                            frame.simulation.challenge, metrics);
                frame.simulation.speedKph = SpeedMetersPerSecond * 3.6;
                frame.feature = runtime.update(frame.simulation);
                WorkoutGamePhysicsInput input;
                input.workoutTimeMs = frame.simulation.workoutTimeMs;
                input.courseDistanceMeters = distance;
                input.terrain = WorkoutGameTerrainKind::RockGarden;
                input.desiredSpeedMetersPerSecond = SpeedMetersPerSecond;
                input.effortRatio = safe ? 0.75 : 1.0;
                input.forceGroundFollowing = safe;
                frame.world = physics.update(input);
                QVERIFY(frame.world.ready);
                QVERIFY(!frame.world.rider.airborne);
                QCOMPARE(frame.world.rider.airHeightMeters(), 0.0);
                const double suspension = 0.5 * (
                        frame.world.rider.rearSuspension
                        + frame.world.rider.frontSuspension);
                if (distance >= piece->challenge.obstacleDistanceMeters
                                + rocks.activeStartMeters
                        && distance <= piece->challenge.obstacleDistanceMeters
                                + rocks.activeEndMeters) {
                    minimumSuspension = std::min(
                            minimumSuspension, suspension);
                    maximumSuspension = std::max(
                            maximumSuspension, suspension);
                }
                maximumLateral = std::max(maximumLateral,
                        std::abs(frame.feature.lateralOffsetMeters));
                if (frameIndex > 0) {
                    const double lateralStep = std::abs(
                            frame.feature.lateralOffsetMeters
                                - previousLateral);
                    QVERIFY2(lateralStep < 0.05,
                             qPrintable(QStringLiteral(
                                 "rock lateral step %1 at frame %2: %3 -> %4")
                                 .arg(lateralStep)
                                 .arg(frameIndex)
                                 .arg(previousLateral)
                                 .arg(frame.feature.lateralOffsetMeters)));
                }
                previousLateral = frame.feature.lateralOffsetMeters;

                window.setFrame(frame,
                        safe ? 165.0 : 225.0, 220.0,
                        safe ? 78 : 88, 150, safe ? 4 : 7);
                QTest::qWait(8);
                if (frame.feature.phase
                            == WorkoutGameFeaturePhase::Action) {
                    auto *rootItem = qobject_cast<QQuickItem *>(
                            window.rootObject());
                    auto *powerValue = rootItem
                            ? findVisualItem(
                                rootItem,
                                QStringLiteral("featurePowerValue"))
                            : nullptr;
                    QVERIFY(powerValue);
                    QVERIFY2(powerValue->property("text").toString()
                                    .contains(QStringLiteral("/ 220 W")),
                             qPrintable(powerValue->property("text")
                                    .toString()));
                }
                const QImage image = window.grabWindow();
                QVERIFY(!image.isNull());
                QVERIFY(sampledColorCount(image) > 30);
                if (!previous.isNull()
                        && changedPixels(previous, image) > 20) {
                    ++visiblyChangedFrames;
                }
                previous = image;
                const QString path = QDir(directory).filePath(
                        QStringLiteral("frame-%1.png")
                            .arg(frameIndex, 4, 10, QLatin1Char('0')));
                QImageWriter writer(path, "png");
                writer.setCompression(1);
                QVERIFY2(writer.write(image), qPrintable(writer.errorString()));
            }
            QVERIFY(visiblyChangedFrames > FrameCount * 4 / 5);
            if (safe) {
                QCOMPARE(maximumLateral, rocks.safeLineLateralMeters);
                safeSuspensionRange = maximumSuspension - minimumSuspension;
            } else {
                QCOMPARE(maximumLateral, 0.0);
                mainSuspensionRange = maximumSuspension - minimumSuspension;
            }
        }
        QVERIFY(mainSuspensionRange > 0.05);
        QVERIFY2(safeSuspensionRange <= mainSuspensionRange * 0.40,
                 qPrintable(QStringLiteral(
                     "safe suspension range %1 exceeded 40 percent of main %2")
                     .arg(safeSuspensionRange).arg(mainSuspensionRange)));
    }

    void exportsSkinnyCompletedAndMissedMainLineMotionFrames()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const QString outputRoot = qEnvironmentVariable(
                "GC_WORKOUT_GAME_SKINNY_VIDEO_DIR");
        if (outputRoot.isEmpty()) {
            QSKIP("Set GC_WORKOUT_GAME_SKINNY_VIDEO_DIR to export frames");
        }
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Skinny);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const auto skinny = WorkoutGameSkinnyGeometry::profile(
                piece->difficulty);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.resize(960, 540);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        constexpr int FrameCount = 288;
        constexpr double SpeedMetersPerSecond = 5.0;
        const double start = piece->challenge.obstacleDistanceMeters
                + skinny.startMeters - 3.0;
        const double end = piece->challenge.obstacleDistanceMeters
                + skinny.endMeters + 3.0;

        for (bool missed : {false, true}) {
            const QString directory = QDir(outputRoot).filePath(
                    missed ? QStringLiteral("missed-main-line")
                           : QStringLiteral("completed-main-line"));
            QVERIFY(QDir().mkpath(directory));
            window.setCourse(course, FtpWatts);
            WorkoutGamePhysics physics;
            QVERIFY(physics.configure(road));
            double previousDistance = -1.0;
            double previousLateral = 0.0;
            double previousElevation = 0.0;
            double maximumVerticalStep = 0.0;
            double maximumRise = 0.0;
            int visiblyChangedFrames = 0;
            QImage previous;
            for (int frameIndex = 0; frameIndex < FrameCount; ++frameIndex) {
                const double progress = double(frameIndex)
                        / double(FrameCount - 1);
                const double distance = start + (end - start) * progress;
                WorkoutGameVisualSnapshot frame = frameAt(road, distance);
                frame.simulation.activeSection = 0;
                frame.simulation.sectionProgress = std::clamp(
                        distance / road.totalLengthMeters, 0.0, 1.0);
                frame.simulation.workoutTimeMs = std::int64_t(std::llround(
                        (distance - start) / SpeedMetersPerSecond * 1000.0));
                frame.simulation.route = WorkoutGameRoute::MainLine;
                frame.simulation.featureOutcome = missed
                        ? WorkoutGameFeatureOutcome::Bypassed
                        : WorkoutGameFeatureOutcome::Completed;
                frame.simulation.challenge = piece->challenge.profile;
                WorkoutGameFeatureChallengeMetrics metrics;
                metrics.averageActualWatts = missed ? 150.0 : 180.0;
                metrics.averageTargetWatts = 175.0;
                metrics.averageEffortRatio = missed ? 150.0 / 175.0
                                                    : 180.0 / 175.0;
                metrics.averageCadenceRpm = missed ? 78.0 : 85.0;
                metrics.averageSpeedKph = SpeedMetersPerSecond * 3.6;
                metrics.averageAdherence = missed ? 0.85 : 1.0;
                frame.simulation.challengeAssessment =
                        WorkoutGameFeatureChallenge::assess(
                            frame.simulation.challenge, metrics);
                frame.simulation.speedKph = SpeedMetersPerSecond * 3.6;
                frame.feature = runtime.update(frame.simulation);
                WorkoutGamePhysicsInput input;
                input.workoutTimeMs = frame.simulation.workoutTimeMs;
                input.courseDistanceMeters = distance;
                input.terrain = WorkoutGameTerrainKind::Skinny;
                input.desiredSpeedMetersPerSecond = SpeedMetersPerSecond;
                input.effortRatio = missed ? 0.75 : 1.0;
                input.forceGroundFollowing = false;
                frame.world = physics.update(input);
                QVERIFY(frame.world.ready);
                QVERIFY(!frame.world.rider.airborne);
                QCOMPARE(frame.world.rider.airHeightMeters(), 0.0);
                QVERIFY(frame.world.rider.distanceMeters > previousDistance);
                if (frameIndex > 0) {
                    maximumVerticalStep = std::max(
                            maximumVerticalStep,
                            std::abs(frame.world.rider.elevationMeters
                                - previousElevation));
                    QVERIFY(std::abs(frame.feature.lateralOffsetMeters
                                - previousLateral) < 0.05);
                }
                previousDistance = frame.world.rider.distanceMeters;
                previousElevation = frame.world.rider.elevationMeters;
                previousLateral = frame.feature.lateralOffsetMeters;
                const WorkoutGameRoadSample sample =
                        WorkoutGameRoadCourseBuilder::sample(road, distance);
                const double datum = sample.center.elevationMeters
                        - sample.surfaceOffsetMeters;
                maximumRise = std::max(
                        maximumRise,
                        frame.world.surfaceElevationMeters - datum);

                window.setFrame(frame,
                        missed ? 150.0 : 180.0, 175.0,
                        missed ? 78 : 85, 148, missed ? 4 : 6);
                QTest::qWait(8);
                const QImage image = window.grabWindow();
                QVERIFY(!image.isNull());
                QVERIFY(sampledColorCount(image) > 30);
                if (frameIndex == FrameCount / 2) {
                    const QRect trailRegion(
                            image.width() * 23 / 100,
                            image.height() * 55 / 100,
                            image.width() * 54 / 100,
                            image.height() * 35 / 100);
                    QVERIFY2(nearColorPixels(
                                image, QColor(QStringLiteral("#78a9bf")),
                                trailRegion) < 10,
                             "Skinny trail contains a clear-color hole");
                }
                if (!previous.isNull()
                        && changedPixels(previous, image) > 20) {
                    ++visiblyChangedFrames;
                }
                previous = image;
                const QString path = QDir(directory).filePath(
                        QStringLiteral("frame-%1.png")
                            .arg(frameIndex, 4, 10, QLatin1Char('0')));
                QImageWriter writer(path, "png");
                writer.setCompression(1);
                QVERIFY2(writer.write(image), qPrintable(writer.errorString()));
            }
            QVERIFY(visiblyChangedFrames > FrameCount * 9 / 10);
            QVERIFY(maximumVerticalStep < 0.10);
            QVERIFY(maximumRise >= skinny.deckHeightMeters * 0.95);
            QVERIFY(std::abs(previousLateral) < 1e-12);
        }
    }

    void exportsRockSlabMainAndSafeLineMotionFrames()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const QString outputRoot = qEnvironmentVariable(
                "GC_WORKOUT_GAME_ROCK_SLAB_VIDEO_DIR");
        if (outputRoot.isEmpty()) {
            QSKIP("Set GC_WORKOUT_GAME_ROCK_SLAB_VIDEO_DIR to export frames");
        }
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::RockSlab);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const WorkoutGameRockSlabGeometryProfile slab =
                WorkoutGameRockSlabGeometry::profile(piece->difficulty);
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));

        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.resize(960, 540);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        constexpr int FrameCount = 288;
        constexpr double SpeedMetersPerSecond = 5.0;
        const double start = piece->challenge.obstacleDistanceMeters
                + slab.startMeters - 5.0;
        const double end = piece->challenge.obstacleDistanceMeters
                + slab.endMeters + 5.0;

        for (WorkoutGameRoute route : {
                 WorkoutGameRoute::MainLine,
                 WorkoutGameRoute::SafeBypass}) {
            const bool safe = route == WorkoutGameRoute::SafeBypass;
            const QString directory = QDir(outputRoot).filePath(
                    safe ? QStringLiteral("safe-line")
                         : QStringLiteral("main-line"));
            QVERIFY(QDir().mkpath(directory));
            window.setCourse(course, FtpWatts);
            WorkoutGamePhysics physics;
            QVERIFY(physics.configure(road));
            double maximumLateral = 0.0;
            double previousLateral = 0.0;
            double previousDistance = -1.0;
            double previousElevation = 0.0;
            double maximumVerticalStep = 0.0;
            double maximumSurfaceRise = 0.0;
            int visiblyChangedFrames = 0;
            QImage previous;
            for (int frameIndex = 0; frameIndex < FrameCount; ++frameIndex) {
                const double progress = double(frameIndex)
                        / double(FrameCount - 1);
                const double distance = start + (end - start) * progress;
                WorkoutGameVisualSnapshot frame = frameAt(road, distance);
                frame.simulation.activeSection = 0;
                frame.simulation.sectionProgress = std::clamp(
                        distance / road.totalLengthMeters, 0.0, 1.0);
                frame.simulation.workoutTimeMs = std::int64_t(std::llround(
                        (distance - start) / SpeedMetersPerSecond * 1000.0));
                frame.simulation.route = route;
                frame.simulation.featureOutcome = safe
                        ? WorkoutGameFeatureOutcome::Bypassed
                        : WorkoutGameFeatureOutcome::Completed;
                frame.simulation.challenge = piece->challenge.profile;
                WorkoutGameFeatureChallengeMetrics metrics;
                metrics.averageActualWatts = safe ? 165.0 : 225.0;
                metrics.averageTargetWatts = 220.0;
                metrics.averageEffortRatio = safe ? 0.75 : 225.0 / 220.0;
                metrics.averageCadenceRpm = safe ? 78.0 : 88.0;
                metrics.averageSpeedKph = SpeedMetersPerSecond * 3.6;
                metrics.averageAdherence = safe ? 0.75 : 1.0;
                frame.simulation.challengeAssessment =
                        WorkoutGameFeatureChallenge::assess(
                            frame.simulation.challenge, metrics);
                frame.simulation.speedKph = SpeedMetersPerSecond * 3.6;
                frame.feature = runtime.update(frame.simulation);
                WorkoutGamePhysicsInput input;
                input.workoutTimeMs = frame.simulation.workoutTimeMs;
                input.courseDistanceMeters = distance;
                input.terrain = WorkoutGameTerrainKind::RockSlab;
                input.desiredSpeedMetersPerSecond = SpeedMetersPerSecond;
                input.effortRatio = safe ? 0.75 : 1.0;
                input.forceGroundFollowing = safe;
                frame.world = physics.update(input);
                QVERIFY(frame.world.ready);
                QVERIFY(!frame.world.rider.airborne);
                QCOMPARE(frame.world.rider.airHeightMeters(), 0.0);
                QVERIFY(frame.world.rider.distanceMeters > previousDistance);
                if (frameIndex > 0) {
                    maximumVerticalStep = std::max(
                            maximumVerticalStep,
                            std::abs(frame.world.rider.elevationMeters
                                - previousElevation));
                }
                previousDistance = frame.world.rider.distanceMeters;
                previousElevation = frame.world.rider.elevationMeters;
                const WorkoutGameRoadSample roadSample =
                        WorkoutGameRoadCourseBuilder::sample(road, distance);
                const double datum = roadSample.center.elevationMeters
                        - roadSample.surfaceOffsetMeters;
                maximumSurfaceRise = std::max(
                        maximumSurfaceRise,
                        frame.world.surfaceElevationMeters - datum);
                maximumLateral = std::max(
                        maximumLateral,
                        std::abs(frame.feature.lateralOffsetMeters));
                if (frameIndex > 0) {
                    QVERIFY(std::abs(frame.feature.lateralOffsetMeters
                                - previousLateral) < 0.05);
                }
                previousLateral = frame.feature.lateralOffsetMeters;

                window.setFrame(frame,
                        safe ? 165.0 : 225.0, 220.0,
                        safe ? 78 : 88, 150, safe ? 4 : 7);
                QTest::qWait(8);
                if (frame.feature.phase
                            == WorkoutGameFeaturePhase::Action) {
                    auto *rootItem = qobject_cast<QQuickItem *>(
                            window.rootObject());
                    auto *powerValue = rootItem
                            ? findVisualItem(
                                rootItem,
                                QStringLiteral("featurePowerValue"))
                            : nullptr;
                    QVERIFY(powerValue);
                    QVERIFY2(powerValue->property("text").toString()
                                    .contains(QStringLiteral("/ 220 W")),
                             qPrintable(powerValue->property("text")
                                    .toString()));
                }
                const QImage image = window.grabWindow();
                QVERIFY(!image.isNull());
                QVERIFY(sampledColorCount(image) > 30);
                if (!previous.isNull()
                        && changedPixels(previous, image) > 20) {
                    ++visiblyChangedFrames;
                }
                previous = image;
                const QString path = QDir(directory).filePath(
                        QStringLiteral("frame-%1.png")
                            .arg(frameIndex, 4, 10, QLatin1Char('0')));
                QImageWriter writer(path, "png");
                writer.setCompression(1);
                QVERIFY2(writer.write(image), qPrintable(writer.errorString()));
            }
            QVERIFY(visiblyChangedFrames > FrameCount * 9 / 10);
            QVERIFY(maximumVerticalStep < 0.12);
            if (safe) {
                QCOMPARE(maximumLateral, slab.safeLineLateralMeters);
                QVERIFY(maximumSurfaceRise <= 0.025);
            } else {
                QCOMPARE(maximumLateral, 0.0);
                QVERIFY(maximumSurfaceRise >= slab.heightMeters * 0.95);
            }
        }
    }

    void exportsAuthoredChallengeCompletedAndBypassedMotionFrames()
    {
        QFETCH(int, terrainValue);
        QFETCH(QByteArray, videoEnvironment);
        QFETCH(double, maximumExpectedAirMeters);
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const QString outputRoot = qEnvironmentVariable(
                videoEnvironment.constData());
        if (outputRoot.isEmpty()) {
            QSKIP("Set the feature video directory to export frames");
        }
        const WorkoutGameTerrainKind terrain =
                WorkoutGameTerrainKind(terrainValue);
        const WorkoutGameCourse course = catalogCourse(
                terrain);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));

        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.resize(960, 540);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        window.setCourse(course, FtpWatts);
        constexpr int FrameCount = 72;
        const bool physicalWorld = terrain == WorkoutGameTerrainKind::Drop
                || terrain == WorkoutGameTerrainKind::Tabletop;
        const double start = std::max(
                0.0, terrain == WorkoutGameTerrainKind::Drop
                    ? piece->challenge.obstacleDistanceMeters - 3.0
                    : piece->challenge.bypassStartDistanceMeters - 2.0);
        const double end = std::min(
                road.totalLengthMeters,
                terrain == WorkoutGameTerrainKind::Drop
                    ? piece->challenge.obstacleDistanceMeters + 5.0
                    : piece->challenge.bypassEndDistanceMeters + 2.0);
        constexpr double SpeedMetersPerSecond = 22.5 / 3.6;

        for (WorkoutGameRoute route : {
                 WorkoutGameRoute::MainLine,
                 WorkoutGameRoute::SafeBypass}) {
            window.setCourse(course, FtpWatts);
            const bool safe = route == WorkoutGameRoute::SafeBypass;
            const QString scenario = safe
                    ? QStringLiteral("bypassed")
                    : QStringLiteral("completed");
            const QString directory = QDir(outputRoot).filePath(scenario);
            QVERIFY(QDir().mkpath(directory));
            WorkoutGamePhysics physics;
            if (physicalWorld) QVERIFY(physics.configure(road));
            QImage previous;
            int changedFrames = 0;
            double largestAirMeters = 0.0;
            for (int frameIndex = 0; frameIndex < FrameCount; ++frameIndex) {
                const double progress = double(frameIndex)
                        / double(FrameCount - 1);
                const double distance = start + (end - start) * progress;
                WorkoutGameVisualSnapshot frame = frameAt(road, distance);
                frame.simulation.activeSection = 0;
                frame.simulation.sectionProgress = std::clamp(
                        distance / road.totalLengthMeters, 0.0, 1.0);
                frame.simulation.route = route;
                frame.simulation.featureOutcome = safe
                        ? WorkoutGameFeatureOutcome::Bypassed
                        : WorkoutGameFeatureOutcome::Completed;
                frame.simulation.speedKph = 22.5;
                frame.simulation.challenge = piece->challenge.profile;
                WorkoutGameFeatureChallengeMetrics metrics;
                metrics.averageActualWatts = safe ? 150.0 : 235.0;
                metrics.averageTargetWatts = 220.0;
                metrics.averageEffortRatio = metrics.averageActualWatts
                        / metrics.averageTargetWatts;
                metrics.averageCadenceRpm = safe ? 72.0 : 92.0;
                metrics.averageSpeedKph = 22.5;
                metrics.averageAdherence = safe
                        ? metrics.averageEffortRatio : 1.0;
                frame.simulation.challengeAssessment =
                        WorkoutGameFeatureChallenge::assess(
                            frame.simulation.challenge, metrics);
                frame.feature = runtime.update(frame.simulation);
                if (physicalWorld) {
                    WorkoutGamePhysicsInput input;
                    input.workoutTimeMs = std::int64_t(std::llround(
                            (distance - start) / SpeedMetersPerSecond
                                * 1000.0));
                    input.courseDistanceMeters = distance;
                    input.terrain = terrain;
                    input.desiredSpeedMetersPerSecond =
                            SpeedMetersPerSecond;
                    input.effortRatio = safe ? 0.5 : 1.0;
                    input.forceGroundFollowing = safe;
                    input.jumpRequested = !safe
                            && frame.feature.triggerJump;
                    input.featureActionId = frame.feature.actionId;
                    frame.world = physics.update(input);
                    QVERIFY(frame.world.ready);
                }
                const double air = physicalWorld
                        ? (frame.world.rider.airborne
                            ? frame.world.rider.airHeightMeters() : 0.0)
                        : frame.feature.verticalOffsetMeters;
                largestAirMeters = std::max(largestAirMeters, air);
                if (!physicalWorld) {
                    frame.world.rider.clearanceMeters = 0.82 + air;
                    frame.world.rider.airborne = air > 0.01;
                    frame.world.rider.pitchDegrees = frame.feature.pitchDegrees;
                }
                window.setFrame(
                        frame,
                        safe ? 150.0 : 235.0,
                        220.0,
                        safe ? 72 : 92,
                        safe ? 145 : 152,
                        safe ? 4 : 7);
                QTest::qWait(12);
                const QImage image = window.grabWindow();
                QVERIFY(!image.isNull());
                QVERIFY(sampledColorCount(image) > 30);
                if (!previous.isNull() && changedPixels(previous, image) > 20) {
                    ++changedFrames;
                }
                previous = image;
                const QString path = QDir(directory).filePath(
                        QStringLiteral("frame-%1.png")
                            .arg(frameIndex, 4, 10, QLatin1Char('0')));
                QImageWriter writer(path, "png");
                writer.setCompression(1);
                QVERIFY2(writer.write(image), qPrintable(writer.errorString()));
            }
            QVERIFY2(changedFrames > FrameCount * 4 / 5,
                     qPrintable(QStringLiteral(
                         "%1 motion changed only %2 of %3 frames")
                         .arg(scenario).arg(changedFrames).arg(FrameCount)));
            if (!safe) {
                QVERIFY(largestAirMeters > 0.20);
                QVERIFY(largestAirMeters <= maximumExpectedAirMeters);
            } else {
                QCOMPARE(largestAirMeters, 0.0);
            }
        }
    }

    void exportsCameraCompositionCatalog()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const QByteArray requestedOutput =
                qgetenv("GC_WORKOUT_GAME_3D_CAMERA_CATALOG_DIR");
        const QString outputDirectory = requestedOutput.isEmpty()
                ? QDir(QDir::tempPath()).filePath(
                    QStringLiteral("workout-game-3d-camera-catalog"))
                : QString::fromLocal8Bit(requestedOutput);
        QVERIFY(QDir().mkpath(outputDirectory));
        const ScopedEnvironmentVariable restore(
                "GC_WORKOUT_GAME_3D_CAMERA");
        const WorkoutGameCourse course = sampleCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const WorkoutGameVisualSnapshot frame = frameAt(
                road, tabletopApproachDistance(road));
        const std::array<QByteArray, 3> compositions = {{
            QByteArrayLiteral("low-centre"),
            QByteArrayLiteral("medium-centre"),
            QByteArrayLiteral("shoulder")
        }};
        QImage prior;

        for (const QByteArray &composition : compositions) {
            qputenv("GC_WORKOUT_GAME_3D_CAMERA", composition);
            WorkoutGame3DWindow window(true);
            QVERIFY(window.rendererAvailable());
            window.setCourse(course, FtpWatts);
            window.setFrame(frame, 220.0, 220.0, 88, 150, 7);
            window.resize(1280, 720);
            window.show();
            QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
            QTest::qWait(350);
            const QImage rendered = window.grabWindow();
            QVERIFY(!rendered.isNull());
            QCOMPARE(rendered.size(), QSize(1280, 720));
            QVERIFY(sampledColorCount(rendered) > 35);
            if (!prior.isNull()) {
                QVERIFY2(changedPixels(prior, rendered) > 500,
                         qPrintable(QStringLiteral(
                             "camera composition %1 is not visually distinct")
                             .arg(QString::fromLatin1(composition))));
            }
            const QString output = QDir(outputDirectory).filePath(
                    QStringLiteral("camera-%1.png")
                        .arg(QString::fromLatin1(composition)));
            QVERIFY2(rendered.save(output), qPrintable(output));
            prior = rendered;
        }

    }

    void exportsCameraCompositionMotionFrames()
    {
        const QByteArray requestedOutput =
                qgetenv("GC_WORKOUT_GAME_3D_CAMERA_VIDEO_DIR");
        if (requestedOutput.isEmpty()) {
            QSKIP("Set GC_WORKOUT_GAME_3D_CAMERA_VIDEO_DIR to export frames");
        }
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }

        constexpr int frameRate = 30;
        constexpr int frameCount = 360;
        constexpr int width = 960;
        constexpr int height = 540;
        const QString outputDirectory =
                QString::fromLocal8Bit(requestedOutput);
        QVERIFY(QDir().mkpath(outputDirectory));
        const ScopedEnvironmentVariable restore(
                "GC_WORKOUT_GAME_3D_CAMERA");
        const WorkoutGameCourse course = cameraMotionCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        QCOMPARE(road.totalLengthMeters, 102.0);
        const double startDistance = 2.0;
        const double endDistance = road.totalLengthMeters - 2.0;
        const double speedKph = (endDistance - startDistance)
                / (double(frameCount - 1) / double(frameRate)) * 3.6;
        const std::array<QByteArray, 3> compositions = {{
            QByteArrayLiteral("low-centre"),
            QByteArrayLiteral("medium-centre"),
            QByteArrayLiteral("shoulder")
        }};

        for (const QByteArray &composition : compositions) {
            qputenv("GC_WORKOUT_GAME_3D_CAMERA", composition);
            const QString compositionDirectory = QDir(outputDirectory).filePath(
                    QString::fromLatin1(composition));
            QVERIFY(QDir().mkpath(compositionDirectory));
            QDir frames(compositionDirectory);
            for (const QString &stale : frames.entryList(
                    {QStringLiteral("frame-*.png")}, QDir::Files)) {
                QVERIFY(frames.remove(stale));
            }

            WorkoutGame3DWindow window(true);
            QVERIFY(window.rendererAvailable());
            window.setCourse(course, FtpWatts);
            window.resize(width, height);
            window.show();
            QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
            QImage prior;
            int visiblyChangedFrames = 0;

            for (int frameIndex = 0;
                 frameIndex < frameCount; ++frameIndex) {
                const double progress = double(frameIndex)
                        / double(frameCount - 1);
                const double distance = startDistance
                        + (endDistance - startDistance) * progress;
                WorkoutGameVisualSnapshot frame = frameAt(road, distance);
                frame.simulation.workoutTimeMs = std::int64_t(std::llround(
                        1000.0 * double(frameIndex) / double(frameRate)));
                frame.simulation.speedKph = speedKph;
                window.setFrame(frame, 220.0, 220.0, 88, 150, 7);
                QTest::qWait(4);

                const QImage rendered = window.grabWindow();
                QVERIFY(!rendered.isNull());
                QCOMPARE(rendered.size(), QSize(width, height));
                QVERIFY2(sampledColorCount(rendered) > 35,
                         "camera motion frame is blank or nearly monochrome");
                if (!prior.isNull()
                        && changedPixels(prior, rendered) > 40) {
                    ++visiblyChangedFrames;
                }
                prior = rendered;

                const QString output = frames.filePath(
                        QStringLiteral("frame-%1.png")
                            .arg(frameIndex, 4, 10, QLatin1Char('0')));
                QImageWriter writer(output, "png");
                writer.setCompression(1);
                QVERIFY2(writer.write(rendered),
                         qPrintable(writer.errorString()));
            }

            QCOMPARE(frames.entryList(
                    {QStringLiteral("frame-*.png")}, QDir::Files).size(),
                     frameCount);
            QVERIFY2(visiblyChangedFrames > frameCount * 9 / 10,
                     qPrintable(QStringLiteral(
                         "%1 camera moved in only %2 of %3 transitions")
                         .arg(QString::fromLatin1(composition))
                         .arg(visiblyChangedFrames)
                         .arg(frameCount - 1)));
        }
    }

    void jumpLiftRemainsVisibleAgainstGroundCamera()
    {
        const WorkoutGameCourse course = sampleCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const double distance = tabletopApproachDistance(road) + 5.0;
        WorkoutGameVisualSnapshot frame = frameAt(road, distance);
        frame.feature.ready = true;
        frame.feature.terrain = WorkoutGameTerrainKind::Tabletop;
        frame.feature.phase = WorkoutGameFeaturePhase::Action;
        frame.feature.motion = WorkoutGameFeatureMotion::Jump;
        frame.feature.outcome = WorkoutGameFeatureOutcome::Completed;
        frame.feature.route = WorkoutGameRoute::MainLine;
        frame.feature.verticalOffsetMeters = 1.15;
        frame.world.rider.clearanceMeters = 0.82 + 1.15;
        frame.world.rider.airborne = true;

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(frame, 260.0, 245.0, 95, 155, 10);

        QVERIFY(viewModel.riderY() - viewModel.groundY() > 1.0);
    }

    void physicsOwnsAirHeightWhenAWorldSnapshotIsAvailable()
    {
        const WorkoutGameCourse course = sampleCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGameVisualSnapshot frame = frameAt(road, 20.0);
        frame.world.rider.clearanceMeters = 0.82 + 0.38;
        frame.world.rider.airborne = true;
        frame.feature.ready = true;
        frame.feature.terrain = WorkoutGameTerrainKind::Tabletop;
        frame.feature.phase = WorkoutGameFeaturePhase::Action;
        frame.feature.motion = WorkoutGameFeatureMotion::Jump;
        frame.feature.outcome = WorkoutGameFeatureOutcome::Completed;
        frame.feature.route = WorkoutGameRoute::MainLine;
        frame.feature.verticalOffsetMeters = 1.35;

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(frame, 260.0, 245.0, 95, 155, 10);

        const WorkoutGameRoadSample sample =
                WorkoutGameRoadCourseBuilder::sample(road, 20.0);
        QVERIFY(sample.ready);
        const double visualGround = sample.center.elevationMeters;
        QVERIFY(std::abs(viewModel.riderY() - visualGround - 0.38) < 1e-9);
    }

    void completedDropKeepsTheNegativeRoadSurfaceOffset()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Drop);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.terrain == WorkoutGameTerrainKind::Drop
                            && candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const double beforeDistance =
                piece->challenge.obstacleDistanceMeters - 1.0;
        const double droppedDistance =
                piece->challenge.obstacleDistanceMeters + 4.0;
        const WorkoutGameRoadSample beforeSample =
                WorkoutGameRoadCourseBuilder::sample(road, beforeDistance);
        const WorkoutGameRoadSample droppedSample =
                WorkoutGameRoadCourseBuilder::sample(road, droppedDistance);
        QVERIFY(beforeSample.ready);
        QVERIFY(droppedSample.ready);
        QVERIFY(droppedSample.center.elevationMeters
                < beforeSample.center.elevationMeters - 0.5);

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        WorkoutGameVisualSnapshot frame = frameAt(road, droppedDistance);
        frame.world.rider.clearanceMeters = 0.82;
        frame.feature.ready = true;
        frame.feature.terrain = WorkoutGameTerrainKind::Drop;
        frame.feature.phase = WorkoutGameFeaturePhase::Action;
        frame.feature.motion = WorkoutGameFeatureMotion::Drop;
        frame.feature.outcome = WorkoutGameFeatureOutcome::Completed;
        frame.feature.route = WorkoutGameRoute::MainLine;
        frame.feature.verticalOffsetMeters = -0.45;
        viewModel.setFrame(frame, 150.0, 150.0, 80, 145, 4);

        QVERIFY(std::abs(viewModel.riderY()
                - droppedSample.center.elevationMeters) < 1e-9);
    }

    void completedDropUsesWorldPitchAsTheSingleAuthority()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Drop);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        WorkoutGameVisualSnapshot frame = frameAt(
                road, piece->challenge.obstacleDistanceMeters + 1.5);
        frame.world.rider.pitchDegrees = -6.0;
        frame.feature.ready = true;
        frame.feature.terrain = WorkoutGameTerrainKind::Drop;
        frame.feature.motion = WorkoutGameFeatureMotion::Drop;
        frame.feature.phase = WorkoutGameFeaturePhase::Action;
        frame.feature.outcome = WorkoutGameFeatureOutcome::Completed;
        frame.feature.pitchDegrees = -14.0;

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(frame, 150.0, 150.0, 80, 145, 4);
        QCOMPARE(viewModel.riderPitch(), -6.0);
    }
};

QTEST_MAIN(TestWorkoutGame3DView)
#include "testWorkoutGame3DView.moc"
