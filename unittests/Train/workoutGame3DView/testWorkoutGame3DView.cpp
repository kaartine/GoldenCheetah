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
#include "WorkoutGameCourseDocument.h"
#include "WorkoutGame3DTerrainProfile.h"
#include "WorkoutGame3DWindow.h"
#include "WorkoutGameDistancePlayback.h"
#include "WorkoutGameFeatureGeometry.h"
#include "WorkoutGameRootGeometry.h"
#include "WorkoutGameRockGardenGeometry.h"
#include "WorkoutGameRockSlabGeometry.h"
#include "WorkoutGameRiderAnimation.h"
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
#include <QHash>
#include <QImage>
#include <QImageWriter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPointer>
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
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

#if defined(Q_OS_LINUX)
#include <sys/resource.h>
#include <time.h>
#endif

namespace {

constexpr double FtpWatts = 200.0;

struct ThreadExecutionSnapshot
{
    qint64 cpuNanoseconds = -1;
    long voluntaryContextSwitches = 0;
    long involuntaryContextSwitches = 0;
};

ThreadExecutionSnapshot threadExecutionSnapshot()
{
    ThreadExecutionSnapshot snapshot;
#if defined(Q_OS_LINUX)
    timespec cpuTime{};
    rusage usage{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpuTime) == 0
            && getrusage(RUSAGE_THREAD, &usage) == 0) {
        snapshot.cpuNanoseconds = qint64(cpuTime.tv_sec) * 1000000000LL
                + qint64(cpuTime.tv_nsec);
        snapshot.voluntaryContextSwitches = usage.ru_nvcsw;
        snapshot.involuntaryContextSwitches = usage.ru_nivcsw;
    }
#endif
    return snapshot;
}

struct FrameUpdateTiming
{
    double wallMilliseconds = 0.0;
    double threadCpuMilliseconds = 0.0;
    long voluntaryContextSwitches = 0;
    long involuntaryContextSwitches = 0;
};

double percentile(std::vector<double> values, double fraction)
{
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const std::size_t rank = std::min(
            values.size() - 1,
            std::size_t(std::ceil(fraction * double(values.size()))) - 1);
    return values[rank];
}

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

WorkoutGameCourse longFlowingMtbCourse()
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 4015825171u;
    course.durationMs = 784200;

    WorkoutGameSection climb;
    climb.feature = WorkoutGameFeature::Climb;
    climb.terrain = WorkoutGameTerrainKind::Climb;
    climb.durationMs = 414000;
    climb.targetWatts = 321.0;
    climb.gradePercent = 7.8875;
    climb.lengthMeters = 1502.7064;
    climb.difficulty = 0.9775;
    climb.visualVariant = 6u;

    WorkoutGameSection descent;
    descent.feature = WorkoutGameFeature::RecoveryDescent;
    descent.terrain = WorkoutGameTerrainKind::Berm;
    descent.startMs = climb.durationMs;
    descent.durationMs = 370200;
    descent.targetWatts = 151.0;
    descent.gradePercent = -1.1431;
    descent.lengthMeters = 2807.6772;
    descent.difficulty = 0.1275;
    descent.visualVariant = 6u;
    descent.gravityAssisted = true;
    course.sections = {climb, descent};
    return course;
}

WorkoutGameCourse sharpTurningCameraCourse()
{
    WorkoutGameCourse course;
    course.status = WorkoutGameCourseStatus::Ready;
    course.seed = 6u;
    course.durationMs = 90000;

    WorkoutGameSection trail;
    trail.feature = WorkoutGameFeature::Trail;
    trail.terrain = WorkoutGameTerrainKind::SmoothTrail;
    trail.durationMs = course.durationMs;
    trail.targetWatts = 225.0;
    trail.lengthMeters = 520.0;
    trail.difficulty = 0.75;
    trail.visualVariant = 3u;
    course.sections = {trail};
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

qint64 settleChaseCamera(
        WorkoutGame3DWindow &window,
        const WorkoutGameRoadCourse &road,
        double destinationMeters,
        double watts,
        double targetWatts,
        int cadenceRpm,
        int heartRateBpm,
        int gear)
{
    constexpr int SettleFrames = 313;
    const double startMeters = std::max(0.0, destinationMeters - 18.0);
    WorkoutGameVisualSnapshot opening = frameAt(road, startMeters);
    opening.simulation.workoutTimeMs = 0;
    opening.presentationTimeMs = 0;
    window.setFrame(
            opening, watts, targetWatts, cadenceRpm, heartRateBpm, gear);
    for (int index = 1; index <= SettleFrames; ++index) {
        const double progress = double(index) / double(SettleFrames);
        WorkoutGameVisualSnapshot frame = frameAt(
                road,
                startMeters + (destinationMeters - startMeters) * progress);
        frame.simulation.workoutTimeMs = index * 16;
        frame.presentationTimeMs = frame.simulation.workoutTimeMs;
        window.setFrame(
                frame, watts, targetWatts, cadenceRpm, heartRateBpm, gear);
    }
    window.update();
    QTest::qWait(700);
    return SettleFrames * 16;
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

QImage withHudMasked(const QImage &source, QQuickItem *rootItem)
{
    QImage masked = source;
    if (masked.isNull() || !rootItem) return masked;
    QPainter painter(&masked);
    for (const QString &name : {
             QStringLiteral("trainingHud"),
             QStringLiteral("diagnosticHud"),
             QStringLiteral("featureHud"),
             QStringLiteral("terrainNameLabel")}) {
        QQuickItem *item = rootItem->findChild<QQuickItem *>(name);
        if (!item || !item->isVisible()) continue;
        const QPointF topLeft = item->mapToItem(rootItem, QPointF(0.0, 0.0));
        const QRect rect = QRectF(
                topLeft, QSizeF(item->width(), item->height()))
                    .toAlignedRect().adjusted(-2, -2, 2, 2)
                    .intersected(masked.rect());
        painter.fillRect(rect, QColor(12, 16, 17));
    }
    return masked;
}

QRect changedPixelBounds(
        const QImage &first,
        const QImage &second,
        const QRect &region)
{
    const QRect bounded = region.intersected(first.rect()).intersected(
            second.rect());
    int minimumX = bounded.right() + 1;
    int minimumY = bounded.bottom() + 1;
    int maximumX = bounded.left() - 1;
    int maximumY = bounded.top() - 1;
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const QColor before(first.pixel(x, y));
            const QColor after(second.pixel(x, y));
            if (std::abs(before.red() - after.red())
                    + std::abs(before.green() - after.green())
                    + std::abs(before.blue() - after.blue()) <= 35) {
                continue;
            }
            minimumX = std::min(minimumX, x);
            minimumY = std::min(minimumY, y);
            maximumX = std::max(maximumX, x);
            maximumY = std::max(maximumY, y);
        }
    }
    return maximumX >= minimumX && maximumY >= minimumY
            ? QRect(QPoint(minimumX, minimumY), QPoint(maximumX, maximumY))
            : QRect();
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

int coniferAssetPixels(const QImage &image, const QRect &region)
{
    int count = 0;
    const QRect bounded = region.intersected(image.rect());
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const QColor color(image.pixel(x, y));
            const bool foliage = color.green() > color.red() + 4
                    && color.green() > color.blue() + 4
                    && color.green() <= 125;
            const bool bark = color.red() > color.green() + 15
                    && color.green() > color.blue() + 8
                    && color.red() <= 180;
            if (foliage || bark) ++count;
        }
    }
    return count;
}

int riderBluePixels(const QImage &image, const QRect &region)
{
    int count = 0;
    const QRect bounded = region.intersected(image.rect());
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const QColor color(image.pixel(x, y));
            if (color.blue() >= color.red() + 30
                    && color.blue() >= color.green() + 20
                    && color.red() <= 110
                    && color.green() <= 145
                    && color.blue() >= 90) {
                ++count;
            }
        }
    }
    return count;
}

int trailDirtPixels(const QImage &image, const QRect &region)
{
    int count = 0;
    const QRect bounded = region.intersected(image.rect());
    for (int y = bounded.top(); y <= bounded.bottom(); y += 2) {
        for (int x = bounded.left(); x <= bounded.right(); x += 2) {
            const QColor color(image.pixel(x, y));
            if (color.red() >= 105
                    && color.red() >= color.green() + 12
                    && color.green() >= color.blue() + 15) {
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
    void activeSessionClockExcludesPausedTimeAndDoesNotResetOnResume()
    {
        WorkoutGameActiveSessionClock clock;
        QCOMPARE(clock.elapsed(900), std::int64_t(0));

        clock.begin(1000);
        QCOMPARE(clock.elapsed(1250), std::int64_t(250));
        clock.pause(1300);
        QCOMPARE(clock.elapsed(5000), std::int64_t(300));

        clock.resume(5000);
        QCOMPARE(clock.elapsed(5200), std::int64_t(500));
        clock.pause(5250);
        clock.pause(6000);
        QCOMPARE(clock.elapsed(7000), std::int64_t(550));

        clock.begin(9000);
        QCOMPARE(clock.elapsed(9050), std::int64_t(50));
    }

    void gearShiftDiagnosticsMeasureTheImmediateSimulationStep()
    {
        WorkoutGameGearShiftDiagnostics diagnostics;
        diagnostics.observe(6, 14.0);
        diagnostics.observe(6, 15.8);
        QCOMPARE(diagnostics.shiftCount(), 0);

        diagnostics.observe(7, 15.95);
        QCOMPARE(diagnostics.shiftCount(), 1);
        QVERIFY(std::abs(diagnostics.maximumSpeedStepKph() - 0.15) < 1e-9);

        diagnostics.observe(7, 18.0);
        diagnostics.observe(6, 17.8);
        QCOMPARE(diagnostics.shiftCount(), 2);
        QVERIFY(std::abs(diagnostics.maximumSpeedStepKph() - 0.20) < 1e-9);

        diagnostics.reset();
        QCOMPARE(diagnostics.shiftCount(), 0);
        QCOMPARE(diagnostics.maximumSpeedStepKph(), 0.0);
    }

    void riderEffortPoseRequiresPowerAndGrade()
    {
        const auto seated = WorkoutGameRiderAnimation::target({
            175.0, 200.0, 1.0, 82.0, false, false
        });
        QVERIFY(seated.pedalEffortBlend > 0.20);
        QVERIFY(seated.standingBlend < 0.01);

        const auto standing = WorkoutGameRiderAnimation::target({
            310.0, 260.0, 10.0, 55.0, false, false
        });
        QVERIFY(standing.pedalEffortBlend > seated.pedalEffortBlend);
        QVERIFY(standing.standingBlend > 0.70);

        const auto spinning = WorkoutGameRiderAnimation::target({
            310.0, 260.0, 10.0, 95.0, false, false
        });
        QVERIFY(spinning.standingBlend < standing.standingBlend);
        QCOMPARE(WorkoutGameRiderAnimation::target({
            310.0, 260.0, 10.0, 55.0, true, false
        }).standingBlend, 0.0);
        QCOMPARE(WorkoutGameRiderAnimation::target({
            310.0, 260.0, 10.0, 55.0, false, true
        }).standingBlend, 0.0);
    }

    void rendererRequestsNonBlockingPresentation()
    {
        WorkoutGame3DWindow window(false);

        QCOMPARE(window.format().swapInterval(), 0);
    }

    void rendererPrewarmCoversSeveralPresentedFrames()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.setCourse(sampleCourse(), FtpWatts);
        window.setCourse(sampleCourse(), FtpWatts);
        auto *viewModel = window.findChild<WorkoutGame3DViewModel *>();
        QVERIFY(viewModel);
        QVERIFY(viewModel->trees().size() >= 12);
        window.resize(960, 540);
        QSignalSpy swaps(&window, &QQuickWindow::frameSwapped);

        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(window.rendererPrewarmed(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(
                window.rootObject()->findChildren<QObject *>(
                    QStringLiteral("workoutGameTree")).size() >= 12,
                5000);

        QVERIFY(swaps.count()
                >= int(WorkoutGame3DWindow::RendererPrewarmFrameCount));

        const int firstCycleSwaps = swaps.count();
        window.setCourse(sampleCourse(), FtpWatts);
        QVERIFY(!window.rendererPrewarmed());
        QTRY_VERIFY_WITH_TIMEOUT(window.rendererPrewarmed(), 5000);
        QVERIFY(swaps.count() - firstCycleSwaps
                >= int(WorkoutGame3DWindow::RendererPrewarmFrameCount));

        window.setCourse(sampleCourse(), FtpWatts);
        QVERIFY(!window.rendererPrewarmed());
        window.setSessionRunning(true);
        QCoreApplication::processEvents();
        QVERIFY(!window.rendererPrewarmed());

        window.setSessionRunning(false);
        const int restartSwaps = swaps.count();
        window.setCourse(sampleCourse(), FtpWatts);
        QTRY_VERIFY_WITH_TIMEOUT(window.rendererPrewarmed(), 5000);
        QVERIFY(swaps.count() - restartSwaps
                >= int(WorkoutGame3DWindow::RendererPrewarmFrameCount));
    }

    void longCourseRemainsVisibleAtMidpointAndFinish()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const WorkoutGameCourse course = longFlowingMtbCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 250.0);
        QVERIFY(road.ready);
        QVERIFY(road.totalLengthMeters > 3000.0);

        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.resize(1280, 720);
        window.setCourse(course, 250.0);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(window.rendererPrewarmed(), 5000);

        double currentDistanceMeters = 0.0;
        constexpr double SimulationStepMeters = 10.0 / 60.0;
        const auto advanceTo = [&](double requestedDistanceMeters) {
            const double destination = std::min(
                    road.totalLengthMeters, requestedDistanceMeters);
            int frames = 0;
            while (currentDistanceMeters + 1.0e-9 < destination) {
                currentDistanceMeters = std::min(
                        destination,
                        currentDistanceMeters + SimulationStepMeters);
                WorkoutGameVisualSnapshot frame = frameAt(
                        road, currentDistanceMeters);
                frame.simulation.workoutTimeMs = qint64(std::llround(
                        currentDistanceMeters / 10.0 * 1000.0));
                frame.presentationTimeMs = frame.simulation.workoutTimeMs;
                window.setFrame(frame, 220.0, 220.0, 88, 150, 7);
                if (++frames % 120 == 0) {
                    QCoreApplication::processEvents();
                }
            }
            QTest::qWait(250);
            window.update();
            QTest::qWait(100);
        };
        const auto verifyVisibleScene = [&](const QString &name) {
            const QImage rendered = window.grabWindow();
            QVERIFY2(!rendered.isNull(), qPrintable(name));
            QVERIFY2(sampledColorCount(rendered) > 35,
                     qPrintable(name + QStringLiteral(" appears blank")));
            const QRect lowerScene(
                    0, rendered.height() * 2 / 5,
                    rendered.width(), rendered.height() * 3 / 5);
            const int clearPixels = nearColorPixels(
                    rendered, QColor(QStringLiteral("#78a9bf")),
                    lowerScene, 8);
            const int sampledPixels =
                    ((lowerScene.width() + 1) / 2)
                    * ((lowerScene.height() + 1) / 2);
            QVERIFY2(clearPixels < sampledPixels * 2 / 3,
                     qPrintable(name
                         + QStringLiteral(" is dominated by clear color")));
            const QVariantList riderPoints = window.rootObject()->property(
                    "riderWheelFrustumScreenPoints").toList();
            QVERIFY2(riderPoints.size() >= 17, qPrintable(name));
            QRectF riderBounds;
            for (const QVariant &pointValue : riderPoints) {
                const QVector3D point = pointValue.value<QVector3D>();
                QVERIFY(std::isfinite(point.x()));
                QVERIFY(std::isfinite(point.y()));
                QVERIFY(std::isfinite(point.z()));
                QVERIFY2(point.z() > 0.0f,
                         qPrintable(name + QStringLiteral(
                             " projects the rider behind the camera")));
                QVERIFY2(point.x() >= 2.0f
                                && point.x() <= rendered.width() - 2.0f
                                && point.y() >= 2.0f
                                && point.y() <= rendered.height() - 2.0f,
                         qPrintable(name + QStringLiteral(
                             " projects the rider outside the frame")));
                riderBounds |= QRectF(point.x(), point.y(), 1.0, 1.0);
            }
            const QRect riderRegion = riderBounds.adjusted(
                    -16.0, -32.0, 16.0, 16.0).toAlignedRect();
            QVERIFY2(riderBluePixels(rendered, riderRegion) > 20,
                     qPrintable(name
                         + QStringLiteral(" does not show the rider")));
            const int trailTop = std::clamp(
                    int(std::floor(riderBounds.bottom() - 8.0)),
                    lowerScene.top(), rendered.height() - 1);
            const QRect trailRegion(
                    rendered.width() * 3 / 10, trailTop,
                    rendered.width() * 4 / 10,
                    rendered.height() - trailTop);
            QVERIFY2(trailDirtPixels(rendered, trailRegion) > 120,
                     qPrintable(name
                         + QStringLiteral(" does not show the trail")));
            const QString outputDirectory = qEnvironmentVariable(
                    "GC_WORKOUT_GAME_3D_LONG_AUDIT_DIR");
            if (!outputDirectory.isEmpty()) {
                QVERIFY(QDir().mkpath(outputDirectory));
                QVERIFY(rendered.save(QDir(outputDirectory).filePath(
                        name + QStringLiteral(".png"))));
            }
        };

        advanceTo(2230.0);
        verifyVisibleScene(QStringLiteral("long-course-2230m"));
        advanceTo(road.totalLengthMeters);
        verifyVisibleScene(QStringLiteral("long-course-finish"));

        WorkoutGameVisualSnapshot beyondFinish = frameAt(
                road, road.totalLengthMeters);
        beyondFinish.world.rider.distanceMeters =
                road.totalLengthMeters + 200.0;
        beyondFinish.simulation.workoutTimeMs = qint64(std::llround(
                road.totalLengthMeters / 10.0 * 1000.0)) + 20000;
        beyondFinish.presentationTimeMs =
                beyondFinish.simulation.workoutTimeMs;
        window.setFrame(beyondFinish, 220.0, 220.0, 88, 150, 7);
        auto *viewModel = qobject_cast<WorkoutGame3DViewModel *>(
                window.rootContext()->contextProperty(
                    QStringLiteral("workoutGame3D")).value<QObject *>());
        QVERIFY(viewModel);
        QCOMPARE(viewModel->distanceMeters(), road.totalLengthMeters);
        QTest::qWait(350);
        verifyVisibleScene(QStringLiteral("long-course-runout"));
    }

    void coldStartCaptureKeepsEverySwapAndVisualRevision()
    {
        WorkoutGameColdStartFrameCapture capture;
        constexpr std::int64_t StartNs = 1000000000ll;
        capture.start(StartNs, 7);

        capture.recordFrame(StartNs + 16000000ll, 7);
        capture.recordFrame(StartNs + 32000000ll, 8);
        capture.recordFrame(StartNs + 48000000ll, 8);
        capture.recordFrame(StartNs + 64000000ll, 9);

        const WorkoutGameColdStartFrameSnapshot snapshot =
                capture.snapshot(StartNs + 80000000ll);
        QVERIFY(snapshot.active);
        QCOMPARE(snapshot.frameCount, std::uint32_t(4));
        QCOMPARE(snapshot.droppedFrameCount, std::uint32_t(0));
        QCOMPARE(snapshot.startToFirstSwapMs, 16.0);
        QCOMPARE(snapshot.startToFirstVisualChangeMs, 32.0);
        QCOMPARE(snapshot.p99FrameIntervalMs, 16.0);
        QCOMPARE(snapshot.maximumFrameIntervalMs, 16.0);
        QCOMPARE(snapshot.maximumConsecutiveLateFrames, std::uint32_t(0));
        QCOMPARE(snapshot.swapFramesPerSecond, 50.0);
        QCOMPARE(snapshot.uniqueVisualFramesPerSecond, 25.0);
        QCOMPARE(snapshot.longestUnchangedVisualIntervalMs, 32.0);
    }

    void fixedStepPresentationUsesRunnerClockEpoch()
    {
        WorkoutGame3DWindow window(false);
        window.setCourse(sampleCourse(), FtpWatts);
        window.setSessionRunning(true);

        const std::int64_t baseTimeMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                .count();
        for (int tick = 0; tick < 3; ++tick) {
            WorkoutGameVisualSnapshot frame;
            frame.presentationTimeMs = baseTimeMs + tick * 20;
            frame.simulation.ready = true;
            frame.simulation.workoutTimeMs = tick * 20;
            frame.world.ready = true;
            frame.world.generation = 1;
            frame.world.rider.distanceMeters = 10.0 + tick * 0.2;
            window.setFrame(frame, 220.0, 220.0, 88, 150, 7);
        }

        QTest::qWait(60);
        QVERIFY(QMetaObject::invokeMethod(
                &window, "presentFrame", Qt::DirectConnection));
        auto *viewModel = qobject_cast<WorkoutGame3DViewModel *>(
                window.rootContext()->contextProperty(
                    QStringLiteral("workoutGame3D")).value<QObject *>());
        QVERIFY(viewModel);
        QVERIFY2(viewModel->distanceMeters() > 10.0,
                 "the renderer sampled a clock epoch before the runner frames");
        QVERIFY(viewModel->distanceMeters() <= 10.4);
        window.setSessionRunning(false);
    }

    void coldStartVisualStallBeginsAfterTheFirstVisualChange()
    {
        WorkoutGameColdStartFrameCapture capture;
        constexpr std::int64_t StartNs = 1500000000ll;
        capture.start(StartNs, 7);

        capture.recordFrame(StartNs + 30000000ll, 7);
        capture.recordFrame(StartNs + 46000000ll, 8);

        const WorkoutGameColdStartFrameSnapshot snapshot =
                capture.snapshot(StartNs + 60000000ll);
        QCOMPARE(snapshot.startToFirstSwapMs, 30.0);
        QCOMPARE(snapshot.startToFirstVisualChangeMs, 46.0);
        QCOMPARE(snapshot.longestUnchangedVisualIntervalMs, 14.0);
    }

    void coldStartCaptureIncludesStartAndConsecutiveLateFrames()
    {
        WorkoutGameColdStartFrameCapture capture;
        constexpr std::int64_t StartNs = 2000000000ll;
        capture.start(StartNs, 1);
        capture.recordFrame(StartNs + 30000000ll, 2);
        capture.recordFrame(StartNs + 61000000ll, 3);
        capture.recordFrame(StartNs + 92000000ll, 4);
        capture.recordFrame(StartNs + 108000000ll, 5);

        const WorkoutGameColdStartFrameSnapshot snapshot =
                capture.snapshot(StartNs + 108000000ll);
        QCOMPARE(snapshot.startToFirstSwapMs, 30.0);
        QCOMPARE(snapshot.p99FrameIntervalMs, 31.0);
        QCOMPARE(snapshot.maximumFrameIntervalMs, 31.0);
        QCOMPARE(snapshot.maximumConsecutiveLateFrames, std::uint32_t(2));
    }

    void coldStartCaptureSeparatesFirstSwapFromFrameIntervals()
    {
        WorkoutGameColdStartFrameCapture capture;
        constexpr std::int64_t StartNs = 2500000000ll;
        capture.start(StartNs, 1);
        capture.recordFrame(StartNs + 62000000ll, 1);
        capture.recordFrame(StartNs + 78000000ll, 2);
        capture.recordFrame(StartNs + 95000000ll, 3);

        const WorkoutGameColdStartFrameSnapshot snapshot =
                capture.snapshot(StartNs + 95000000ll);
        QCOMPARE(snapshot.startToFirstSwapMs, 62.0);
        QCOMPARE(snapshot.p99FrameIntervalMs, 17.0);
        QCOMPARE(snapshot.maximumFrameIntervalMs, 17.0);
        QCOMPARE(snapshot.maximumConsecutiveLateFrames, std::uint32_t(0));
    }

    void coldStartCaptureReportsFixedCapacityOverflow()
    {
        WorkoutGameColdStartFrameCapture capture;
        capture.start(0, 0);
        for (std::size_t index = 0;
             index < WorkoutGameColdStartFrameCapture::Capacity + 3;
             ++index) {
            capture.recordFrame(
                    std::int64_t(index + 1) * 1000000ll,
                    std::uint64_t(index + 1));
        }

        const WorkoutGameColdStartFrameSnapshot snapshot =
                capture.snapshot(5000000000ll);
        QCOMPARE(snapshot.frameCount,
                 std::uint32_t(WorkoutGameColdStartFrameCapture::Capacity));
        QCOMPARE(snapshot.droppedFrameCount, std::uint32_t(3));
    }

    void streamingCoverageRefreshesOnlyNearAnEdge()
    {
        const WorkoutGame3DStreamingCoverage coverage{0.0, 130.0};
        QVERIFY(coverage.valid());
        QVERIFY(!coverage.needsRefresh(0.0, 500.0, 15.0, 50.0));
        QVERIFY(!coverage.needsRefresh(79.9, 500.0, 15.0, 50.0));
        QVERIFY(coverage.needsRefresh(80.1, 500.0, 15.0, 50.0));
        const WorkoutGame3DStreamingCoverage finalCoverage{350.0, 500.0};
        QVERIFY(!finalCoverage.needsRefresh(
                    470.0, 500.0, 15.0, 50.0));
        QVERIFY(coverage.needsRefresh(-0.1, 500.0, 15.0, 50.0));
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
        QCOMPARE(geometry->sampleCount(), 2352);
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

    void ordinaryResidentFramesDoNotRegenerateModels()
    {
        const WorkoutGameCourse course = longFlowingMtbCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        WorkoutGameVisualSnapshot frame = frameAt(road, 8.0);
        frame.simulation.workoutTimeMs = 5000;
        frame.presentationTimeMs = 5000;
        viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
        viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
        QCoreApplication::processEvents();

        QSignalSpy courseSignals(
                &viewModel, &WorkoutGame3DViewModel::courseChanged);
        QSignalSpy treeSignals(
                &viewModel, &WorkoutGame3DViewModel::treesChanged);
        QSignalSpy forestSignals(
                &viewModel, &WorkoutGame3DViewModel::forestDressingChanged);
        QSignalSpy floorSignals(
                &viewModel, &WorkoutGame3DViewModel::floorGeometryChanged);
        QSignalSpy telemetrySignals(
                &viewModel, &WorkoutGame3DViewModel::telemetryChanged);
        QSignalSpy sceneSignals(
                &viewModel, &WorkoutGame3DViewModel::sceneChanged);
        viewModel.resetFrameWorkCounters();

        constexpr int OrdinaryFrames = 500;
        for (int index = 0; index < OrdinaryFrames; ++index) {
            viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
        }

        const WorkoutGame3DFrameWorkCounters counters =
                viewModel.frameWorkCounters();
        qInfo().noquote() << QStringLiteral(
                "ordinary resident work: frames=%1 scene=%2 telemetry=%3 "
                "course/tree/forest/floor=%4/%5/%6/%7; "
                "regenerations=%8/%9/%10; floor requests/installs=%11/%12; "
                "tree entries visited=%13")
            .arg(counters.frameCalls)
            .arg(counters.sceneSignals)
            .arg(counters.telemetrySignals)
            .arg(counters.courseSignals)
            .arg(counters.treeSignals)
            .arg(counters.forestSignals)
            .arg(counters.floorSignals)
            .arg(counters.featureModelRegenerations)
            .arg(counters.treeModelRegenerations)
            .arg(counters.forestModelRegenerations)
            .arg(counters.floorBuildRequests)
            .arg(counters.floorChunkInstalls)
            .arg(counters.treeClearanceEntriesVisited);
        QCOMPARE(counters.frameCalls, std::uint64_t(OrdinaryFrames));
        QCOMPARE(counters.sceneSignals, std::uint64_t(OrdinaryFrames));
        QCOMPARE(counters.telemetrySignals, std::uint64_t(0));
        QCOMPARE(counters.floorBuildRequests, std::uint64_t(0));
        QCOMPARE(counters.floorChunkInstalls, std::uint64_t(0));
        QCOMPARE(counters.featureModelRegenerations, std::uint64_t(0));
        QCOMPARE(counters.treeModelRegenerations, std::uint64_t(0));
        QCOMPARE(counters.forestModelRegenerations, std::uint64_t(0));
        QVERIFY(counters.treeClearanceEntriesVisited > 0);
        QVERIFY(counters.treeClearanceEntriesVisited
                <= std::uint64_t(OrdinaryFrames * 18));
        QCOMPARE(courseSignals.count(), 0);
        QCOMPARE(treeSignals.count(), 0);
        QCOMPARE(forestSignals.count(), 0);
        QCOMPARE(floorSignals.count(), 0);
        QCOMPARE(telemetrySignals.count(), 0);
        QCOMPARE(sceneSignals.count(), OrdinaryFrames);
    }

    void readyFloorChunkIsNotInstalledBySetFrame()
    {
        const WorkoutGameCourse course = longFlowingMtbCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.resetFrameWorkCounters();
        WorkoutGameVisualSnapshot frame = frameAt(road, 260.0);
        frame.simulation.workoutTimeMs = 10000;
        frame.presentationTimeMs = 10000;
        viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
        QCOMPARE(viewModel.frameWorkCounters().floorBuildRequests,
                 std::uint64_t(1));

        QElapsedTimer completionWait;
        completionWait.start();
        while (viewModel.frameWorkCounters().floorChunkBuildsCompleted == 0
                && completionWait.elapsed() < 5000) {
            QThread::msleep(2);
        }
        QCOMPARE(viewModel.frameWorkCounters().floorChunkBuildsCompleted,
                 std::uint64_t(1));
        QCOMPARE(viewModel.frameWorkCounters().floorChunkInstalls,
                 std::uint64_t(0));

        QSignalSpy floorSignals(
                &viewModel, &WorkoutGame3DViewModel::floorGeometryChanged);
        viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
        QCOMPARE(viewModel.frameWorkCounters().floorChunkInstalls,
                 std::uint64_t(0));
        QCOMPARE(floorSignals.count(), 0);

        QTRY_COMPARE_WITH_TIMEOUT(
                viewModel.frameWorkCounters().floorChunkInstalls,
                std::uint64_t(1), 3000);
        QCOMPARE(floorSignals.count(), 1);
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
        QVERIFY(viewModel->trees().size() <= 14);
        QCOMPARE(viewModel->forestFloorProps().size(), 4);
        QCOMPARE(viewModel->forestVergeClusters().size(), 3);
        QVERIFY(viewModel->geometryQueueDepth() <= 1);

        QObject *view = window.rootObject()->findChild<QObject *>(
                QStringLiteral("workoutGame3DView"));
        QVERIFY(view);
        QObject *stats = view->property("renderStats").value<QObject *>();
        QVERIFY(stats);
        QTRY_VERIFY_WITH_TIMEOUT(
                stats->property("drawCallCount").toULongLong() > 0, 5000);
        QVERIFY2(stats->property("drawCallCount").toULongLong() <= 80,
                 qPrintable(QStringLiteral("draw-call budget exceeded: %1")
                         .arg(stats->property("drawCallCount")
                              .toULongLong())));
        window.setSessionRunning(false);
    }

    void groundShadowOnlyRendersWithAirClearance()
    {
        const WorkoutGameCourse course = sampleCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        WorkoutGameVisualSnapshot frame = frameAt(road, 12.0);
        viewModel.setFrame(frame, 215.0, 220.0, 87, 148, 7);

        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        window.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        QObject *shadow = window.rootObject()->findChild<QObject *>(
                QStringLiteral("riderGroundShadow"));
        QVERIFY(shadow);
        QCOMPARE(shadow->property("visible").toBool(), false);

        window.rootObject()->setProperty("rendererPrewarming", true);
        QTRY_COMPARE(shadow->property("visible").toBool(), true);
        window.rootObject()->setProperty("rendererPrewarming", false);
        QTRY_COMPARE(shadow->property("visible").toBool(), false);

        frame.world.rider.airborne = true;
        frame.world.rider.clearanceMeters = 1.20;
        frame.simulation.workoutTimeMs += 16;
        viewModel.setFrame(frame, 215.0, 220.0, 87, 148, 7);
        QTRY_COMPARE(shadow->property("visible").toBool(), true);
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
        QVERIFY(trace.contains(QStringLiteral("session_elapsed_ms=")));
        QVERIFY(trace.contains(QStringLiteral("p50_frame_ms=")));
        QVERIFY(trace.contains(QStringLiteral("p95_frame_ms=")));
        QVERIFY(trace.contains(QStringLiteral("p99_frame_ms=")));
        QVERIFY(trace.contains(QStringLiteral(
                "cold_start_first_visual_ms=")));
        QVERIFY(trace.contains(QStringLiteral("frame_ms=")));
        QVERIFY(trace.contains(QStringLiteral("timing_warm=")));
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
        QVERIFY(trace.contains(
                QStringLiteral("camera_presentation=chase")));
        QVERIFY(trace.contains(QStringLiteral("camera_side_blend=0")));
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

        window.beginTrainingSessionTiming();
        window.setSessionRunning(true);
        QTest::qWait(100);
        QVERIFY(!window.diagnosticsSnapshot().ready);

        WorkoutGameVisualSnapshot restarted = frameAt(road, 2.0);
        restarted.simulation.workoutTimeMs = 100;
        window.setFrame(restarted, 180.0, 190.0, 82, 140, 6);
        QTRY_VERIFY_WITH_TIMEOUT(
                window.diagnosticsSnapshot().ready
                && window.diagnosticsSnapshot().input
                    .renderedRoadDistanceMeters < 4.0,
                3000);
        window.setSessionRunning(false);
    }

    void movingVisualStateTracksDeliveredSourceFrames()
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
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(window.rendererPrewarmed(), 5000);

        QElapsedTimer clock;
        const qint64 presentationEpochMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                .count();
        QTimer inputTimer;
        inputTimer.setTimerType(Qt::PreciseTimer);
        inputTimer.setInterval(20);
        qint64 previousInputMs = -1;
        qint64 maximumInputGapMs = 0;
        int inputFrames = 0;
        QObject::connect(&inputTimer, &QTimer::timeout, &window, [&]() {
            const qint64 elapsedMs = clock.elapsed();
            if (previousInputMs >= 0) {
                maximumInputGapMs = std::max(
                        maximumInputGapMs, elapsedMs - previousInputMs);
            }
            previousInputMs = elapsedMs;
            ++inputFrames;
            WorkoutGameVisualSnapshot frame = frameAt(
                    road, std::min(18.0, double(elapsedMs) * 0.009));
            frame.simulation.workoutTimeMs = elapsedMs;
            frame.presentationTimeMs = presentationEpochMs + elapsedMs;
            window.setFrame(frame, 220.0, 220.0, 88, 150, 7);
        });

        WorkoutGameVisualSnapshot initial = frameAt(road, 0.0);
        initial.simulation.workoutTimeMs = 0;
        initial.presentationTimeMs = presentationEpochMs;
        window.setFrame(initial, 220.0, 220.0, 88, 150, 7);
        clock.start();
        inputTimer.start();
        window.setSessionRunning(true);
        QTRY_VERIFY_WITH_TIMEOUT(
                window.diagnosticsSnapshot().input.coldStart.frameCount > 60,
                3000);
        inputTimer.stop();

        const WorkoutGameColdStartFrameSnapshot capture =
                window.diagnosticsSnapshot().input.coldStart;
        QVERIFY(capture.swapFramesPerSecond > 30.0);
        QVERIFY2(capture.uniqueVisualFramesPerSecond > 20.0,
                 qPrintable(QStringLiteral(
                         "only %1 unique visual frames/s at %2 swaps/s")
                         .arg(capture.uniqueVisualFramesPerSecond)
                         .arg(capture.swapFramesPerSecond)));
        const double permittedVisualStallMs = std::max(
                50.0, double(maximumInputGapMs) + 25.0);
        QVERIFY2(capture.longestUnchangedVisualIntervalMs
                    <= permittedVisualStallMs,
                 qPrintable(QStringLiteral(
                         "visual state stalled for %1 ms; %2 unique/s, "
                         "%3 swaps/s, %4 source frames, max source gap %5 ms, "
                         "permitted %6 ms")
                         .arg(capture.longestUnchangedVisualIntervalMs)
                         .arg(capture.uniqueVisualFramesPerSecond)
                         .arg(capture.swapFramesPerSecond)
                         .arg(inputFrames)
                         .arg(maximumInputGapMs)
                         .arg(permittedVisualStallMs)));
        window.setSessionRunning(false);
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

    void cameraCompositionIsTheApprovedMediumCentreView()
    {
        const ScopedEnvironmentVariable restore(
                "GC_WORKOUT_GAME_3D_CAMERA");

        qputenv("GC_WORKOUT_GAME_3D_CAMERA", "low-centre");
        WorkoutGame3DViewModel viewModel;
        QCOMPARE(viewModel.cameraComposition(),
                 QStringLiteral("medium-centre"));
        QCOMPARE(viewModel.cameraSideMeters(), 0.0);
        QCOMPARE(viewModel.cameraBackMeters(), 8.2);
        QCOMPARE(viewModel.cameraHeightMeters(), 3.2);
        QCOMPARE(viewModel.cameraLookAheadMeters(), 12.0);
        QCOMPARE(viewModel.cameraTargetHeightMeters(), 0.85);
    }

    void cameraIntroducesTheBikeFromTheSideBeforeReturningToChase()
    {
        const WorkoutGameCourse course = cameraMotionCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        constexpr double distanceMeters = 18.0;
        const WorkoutGameRoadSample rider =
                WorkoutGameRoadCourseBuilder::sample(road, distanceMeters);
        QVERIFY(rider.ready);
        const double rightX = std::cos(rider.center.headingRadians);
        const double rightZ = -std::sin(rider.center.headingRadians);
        const auto cameraLateralMeters = [&](const WorkoutGame3DViewModel &model) {
            return (model.cameraX() - rider.center.xMeters) * rightX
                    + (model.cameraZ() - rider.center.zMeters) * rightZ;
        };

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        WorkoutGameVisualSnapshot frame = frameAt(road, distanceMeters);
        frame.simulation.workoutTimeMs = 0;
        frame.presentationTimeMs = 0;
        viewModel.setFrame(frame, 0.0, 180.0, 0, 80, 5);
        QVERIFY2(std::abs(cameraLateralMeters(viewModel)) > 3.0,
                 "the opening camera did not reveal the bike from the side");
        QCOMPARE(viewModel.cameraPresentation(),
                 QStringLiteral("opening-side"));
        QCOMPARE(viewModel.cameraPresentationBlend(), 1.0);
        const WorkoutGame3DTerrainProfileSnapshot terrain =
                WorkoutGame3DTerrainProfile::build(
                    rider, distanceMeters, road.seed);
        QVERIFY(terrain.ready);
        const double sideSurface =
                WorkoutGame3DTerrainProfile::elevationAtLateral(
                    terrain, -4.4);
        QVERIFY2(viewModel.cameraY() - sideSurface >= 2.54,
                 "the side camera entered the terrain exclusion height");

        frame.simulation.workoutTimeMs = 3000;
        frame.presentationTimeMs = 3000;
        viewModel.setFrame(frame, 180.0, 180.0, 85, 120, 5);
        QCOMPARE(viewModel.cameraPresentation(),
                 QStringLiteral("opening-side"));
        QCOMPARE(viewModel.cameraPresentationBlend(), 1.0);

        frame.simulation.workoutTimeMs = 5000;
        frame.presentationTimeMs = 5000;
        viewModel.setFrame(frame, 180.0, 180.0, 85, 120, 5);
        QCOMPARE(viewModel.cameraPresentation(), QStringLiteral("chase"));
        QCOMPARE(viewModel.cameraPresentationBlend(), 0.0);
    }

    void cameraSidePresentationFiltersStopsAndYieldsToGameplay()
    {
        WorkoutGame3DCameraPresentation presentation;
        QCOMPARE(presentation.update({0, 0.0, 0.0, false, false}).mode,
                 WorkoutGame3DCameraPresentationMode::OpeningSide);
        QCOMPARE(presentation.update({5000, 180.0, 85.0, false, false})
                         .sideBlend,
                 0.0);

        QCOMPARE(presentation.update({5100, 0.0, 0.0, false, false})
                         .sideBlend,
                 0.0);
        QCOMPARE(presentation.update({9099, 0.0, 0.0, false, false})
                         .sideBlend,
                 0.0);
        const auto entering = presentation.update(
                {10000, 0.0, 0.0, false, false});
        QCOMPARE(entering.mode,
                 WorkoutGame3DCameraPresentationMode::IdleSide);
        QVERIFY(entering.sideBlend > 0.0);
        QVERIFY(entering.sideBlend < 1.0);
        QCOMPARE(presentation.update({10900, 0.0, 0.0, false, false})
                         .sideBlend,
                 1.0);

        const auto returning = presentation.update(
                {10950, 170.0, 82.0, false, false});
        QCOMPARE(returning.mode,
                 WorkoutGame3DCameraPresentationMode::ReturningToChase);
        QVERIFY(returning.sideBlend > 0.0);
        QCOMPARE(presentation.update({12150, 170.0, 82.0, false, false})
                         .sideBlend,
                 0.0);

        presentation.reset();
        const auto safety = presentation.update(
                {0, 0.0, 0.0, true, false});
        QCOMPARE(safety.mode, WorkoutGame3DCameraPresentationMode::Chase);
        QCOMPARE(safety.sideBlend, 0.0);
        QCOMPARE(presentation.update({500, 0.0, 0.0, false, false})
                         .sideBlend,
                 0.0);
    }

    void cameraFollowsRoadAndMaintainsTerrainClearance()
    {
        const WorkoutGameCourse course = cameraMotionCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        WorkoutGameVisualSnapshot opening = frameAt(road, 0.0);
        opening.simulation.workoutTimeMs = 0;
        viewModel.setFrame(opening, 220.0, 220.0, 88, 150, 7);

        for (double distance = 0.0;
             distance <= road.totalLengthMeters; distance += 1.0) {
            WorkoutGameVisualSnapshot frame = frameAt(road, distance);
            frame.simulation.workoutTimeMs = 5000
                    + std::int64_t(std::llround(distance * 50.0));
            viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
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
            WorkoutGameVisualSnapshot frame = frameAt(road, distance);
            frame.simulation.workoutTimeMs =
                    std::int64_t(std::llround(distance * 100.0));
            viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
            QVERIFY(viewModel.trees().size() <= 18);
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

    void nearForestFillsBothSidesOfTheSingletrack()
    {
        const WorkoutGameCourse course = cameraMotionCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(frameAt(road, 42.0), 220.0, 220.0, 88, 150, 7);

        QVERIFY2(viewModel.trees().size() >= 12,
                 "the near forest still reads as sparse roadside props");
        bool left = false;
        bool right = false;
        for (const QVariant &entry : viewModel.trees()) {
            const double lateral = entry.toMap().value(
                    QStringLiteral("lateral")).toDouble();
            left = left || lateral < 0.0;
            right = right || lateral > 0.0;
        }
        QVERIFY2(left && right,
                 "the visible forest does not enclose both sides of the trail");
    }

    void coursePrewarmPublishesForestBeforeTheFirstLiveFrame()
    {
        const WorkoutGameCourse course = cameraMotionCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;

        viewModel.setCourse(course, FtpWatts);

        QVERIFY2(viewModel.trees().size() >= 12,
                 "course prewarm leaves the expensive forest repeater empty");
        QVERIFY(viewModel.trees().size() <= 18);

        QSignalSpy changes(&viewModel, &WorkoutGame3DViewModel::treesChanged);
        WorkoutGameVisualSnapshot first = frameAt(road, 0.0);
        first.simulation.workoutTimeMs = 0;
        viewModel.setFrame(first, 100.0, 100.0, 80, 130, 6);
        QCOMPARE(changes.count(), 0);

        WorkoutGameCourse invalid;
        viewModel.setCourse(invalid, FtpWatts);
        QVERIFY(viewModel.trees().isEmpty());
        QCOMPARE(changes.count(), 1);
    }

    void featureCriticalFirstFrameKeepsThePrewarmedForest()
    {
        const WorkoutGameCourse course = cameraMotionCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        QVERIFY(viewModel.trees().size() >= 12);
        QSignalSpy changes(&viewModel, &WorkoutGame3DViewModel::treesChanged);

        WorkoutGameVisualSnapshot first = frameAt(road, 0.0);
        first.simulation.workoutTimeMs = 0;
        first.feature.ready = true;
        first.feature.phase = WorkoutGameFeaturePhase::Action;
        viewModel.setFrame(first, 260.0, 220.0, 90, 150, 7);

        QCOMPARE(viewModel.cameraPresentation(), QStringLiteral("chase"));
        QCOMPARE(changes.count(), 0);
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

    void forestDressingIsBoundedGroundedAndOutsideTheSingletrack()
    {
        const WorkoutGameCourse course = cameraMotionCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(frameAt(road, 42.0), 220.0, 220.0, 88, 150, 7);

        const QVariantList floorProps = viewModel.forestFloorProps();
        const QVariantList vergeClusters = viewModel.forestVergeClusters();
        QCOMPARE(floorProps.size(), 4);
        QCOMPARE(vergeClusters.size(), 3);
        QCOMPARE(floorProps.size() + vergeClusters.size(), 7);

        const auto verifyPlacement = [&road, &viewModel](
                const QVariant &entry, int maximumVariant) {
            const QVariantMap prop = entry.toMap();
            const double distance = prop.value(
                    QStringLiteral("distance")).toDouble();
            const double lateral = prop.value(
                    QStringLiteral("lateral")).toDouble();
            const WorkoutGameRoadSample sample =
                    WorkoutGameRoadCourseBuilder::sampleVisual(
                            road, distance);
            QVERIFY(sample.ready);
            QVERIFY(std::abs(lateral)
                    >= sample.center.halfWidthMeters + 1.70);
            const WorkoutGame3DTerrainProfileSnapshot terrain =
                    WorkoutGame3DTerrainProfile::build(
                            sample, distance, road.seed);
            QVERIFY(terrain.ready);
            const double expectedY =
                    WorkoutGame3DTerrainProfile::elevationAtLateral(
                            terrain, lateral);
            QVERIFY(std::abs(prop.value(QStringLiteral("y")).toDouble()
                             - expectedY) < 1.0e-9);
            QVERIFY(std::isfinite(
                    prop.value(QStringLiteral("yaw")).toDouble()));
            QVERIFY(std::isfinite(
                    prop.value(QStringLiteral("terrainRoll")).toDouble()));
            QVERIFY(prop.value(QStringLiteral("scale")).toDouble() >= 0.72);
            QVERIFY(prop.value(QStringLiteral("scale")).toDouble() <= 1.08);
            const int variant = prop.value(
                    QStringLiteral("variant")).toInt();
            QVERIFY(variant >= 0);
            QVERIFY(variant <= maximumVariant);
            QCOMPARE(prop.value(QStringLiteral("mirror")).toBool(),
                     lateral < 0.0);

            const double clearance = horizontalDistanceToSegment(
                    prop.value(QStringLiteral("x")).toDouble(),
                    prop.value(QStringLiteral("z")).toDouble(),
                    viewModel.cameraX(), viewModel.cameraZ(),
                    viewModel.cameraTargetX(), viewModel.cameraTargetZ());
            QVERIFY2(clearance >= 1.70,
                     "forest dressing entered the camera/cue corridor");
        };
        for (const QVariant &entry : floorProps) verifyPlacement(entry, 7);
        for (const QVariant &entry : vergeClusters) verifyPlacement(entry, 2);
    }

    void forestDressingStaysOutsideAuthoredGapJumpGround()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 1701u;
        course.durationMs = 30000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::SprintJump;
        section.terrain = WorkoutGameTerrainKind::GapJump;
        section.durationMs = course.durationMs;
        section.lengthMeters = 120.0;
        section.targetWatts = 260.0;
        section.difficulty = 0.5;
        section.challengeCount = 1;
        course.sections = {section};

        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const auto gate = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &piece) {
                    return piece.gapJump.enabled;
                });
        QVERIFY(gate != road.pieces.end());

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        const double reviewDistance = 0.5 * (
                gate->gapJump.splitStartDistanceMeters
                + gate->gapJump.mergeEndDistanceMeters);
        viewModel.setFrame(
                frameAt(road, reviewDistance), 260.0, 260.0, 92, 154, 8);
        QCOMPARE(viewModel.gapJumpFeatures().size(), 1);
        QVERIFY(!viewModel.forestFloorProps().isEmpty());
        QVERIFY(!viewModel.forestVergeClusters().isEmpty());

        const auto verifyOutside = [&gate](const QVariant &entry) {
            const double distance = entry.toMap().value(
                    QStringLiteral("distance")).toDouble();
            QVERIFY2(distance < gate->gapJump.splitStartDistanceMeters
                        || distance > gate->gapJump.mergeEndDistanceMeters,
                     "forest dressing entered authored gap-jump ground");
        };
        for (const QVariant &entry : viewModel.forestFloorProps()) {
            verifyOutside(entry);
        }
        for (const QVariant &entry : viewModel.forestVergeClusters()) {
            verifyOutside(entry);
        }
    }

    void mixedFeatureCourseInstantiatesOnlyActualGapAssets()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 2701u;
        const std::array<WorkoutGameTerrainKind, 2> terrains = {{
            WorkoutGameTerrainKind::GapJump,
            WorkoutGameTerrainKind::BunnyHop
        }};
        std::int64_t startMs = 0;
        for (const WorkoutGameTerrainKind terrain : terrains) {
            WorkoutGameSection section;
            section.feature = WorkoutGameFeature::SprintJump;
            section.terrain = terrain;
            section.startMs = startMs;
            section.durationMs = 30000;
            section.lengthMeters = 80.0;
            section.targetWatts = 250.0;
            section.difficulty = 0.5;
            section.challengeCount = 1;
            course.sections.push_back(section);
            startMs += section.durationMs;
        }
        course.durationMs = startMs;

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        viewModel.setFrame(
                frameAt(road, 0.0), 250.0, 250.0, 90, 152, 8);

        int expectedGapAssets = 0;
        for (const QVariant &entry : viewModel.features()) {
            expectedGapAssets += entry.toMap().value(
                    QStringLiteral("kind")).toInt()
                    == int(WorkoutGameTerrainKind::GapJump);
        }
        QVERIFY(viewModel.features().size() > expectedGapAssets);
        QVERIFY(expectedGapAssets > 0);
        QCOMPARE(viewModel.gapJumpFeatures().size(), expectedGapAssets);

        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.resize(960, 540);
        window.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        window.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        const QList<QObject *> assets =
                window.rootObject()->findChildren<QObject *>(
                    QStringLiteral("gapJumpAssetInstance"));
        QCOMPARE(assets.size(), expectedGapAssets);
        for (QObject *asset : assets) {
            QVERIFY(asset->property("visible").toBool());
        }
    }

    void forestDressingDelegatesUseResidentEdgeFades()
    {
        const WorkoutGameCourse course = cameraMotionCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(frameAt(road, 42.0), 220.0, 220.0, 88, 150, 7);

        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.resize(960, 540);
        window.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        window.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        QTest::qWait(500);

        const QList<QObject *> floorProps =
                window.rootObject()->findChildren<QObject *>(
                    QStringLiteral("workoutGameForestFloorProp"));
        const QList<QObject *> vergeClusters =
                window.rootObject()->findChildren<QObject *>(
                    QStringLiteral("workoutGameForestVergeCluster"));
        QCOMPARE(floorProps.size(), viewModel.forestFloorProps().size());
        QCOMPARE(vergeClusters.size(),
                 viewModel.forestVergeClusters().size());
        QVERIFY(floorProps.size() + vergeClusters.size() <= 7);
        bool foundOpaque = false;
        for (QObject *object : floorProps + vergeClusters) {
            const double relative = object->property(
                    "relativeDistance").toDouble();
            const double opacity = object->property("opacity").toDouble();
            QVERIFY(opacity >= 0.0);
            QVERIFY(opacity <= 1.0);
            if (relative >= -10.0 && relative <= 31.0
                    && opacity > 0.95) {
                foundOpaque = true;
            }
        }
        QVERIFY(foundOpaque);

        const auto edgeOpacity = [&window](double distance) {
            QVariant result;
            const bool invoked = QMetaObject::invokeMethod(
                    window.rootObject(), "dressingEdgeOpacity",
                    Q_RETURN_ARG(QVariant, result),
                    Q_ARG(QVariant, QVariant(distance)));
            return invoked ? result.toDouble() : -1.0;
        };
        QCOMPARE(edgeOpacity(-14.0), 0.0);
        QCOMPARE(edgeOpacity(-10.0), 1.0);
        QCOMPARE(edgeOpacity(36.0), 1.0);
        QCOMPARE(edgeOpacity(44.0), 0.0);
    }

    void forestDressingOnlyChangesAtTransparentResidentEdges()
    {
        const WorkoutGameCourse course = longFlowingMtbCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);

        const auto placements = [&viewModel]() {
            QHash<qint64, QVariantMap> result;
            for (const QVariant &entry : viewModel.forestFloorProps()
                    + viewModel.forestVergeClusters()) {
                const QVariantMap prop = entry.toMap();
                result.insert(qRound64(
                        prop.value(QStringLiteral("distance")).toDouble()
                            * 1000.0), prop);
            }
            return result;
        };

        QHash<qint64, QVariantMap> previous;
        double previousDistance = 0.0;
        for (int step = 0; step <= 120; ++step) {
            const double distance = 20.0 + 0.5 * step;
            WorkoutGameVisualSnapshot frame = frameAt(road, distance);
            frame.simulation.workoutTimeMs = 5000 + step * 50;
            frame.presentationTimeMs = frame.simulation.workoutTimeMs;
            viewModel.setFrame(frame, 225.0, 225.0, 88, 150, 7);
            const QHash<qint64, QVariantMap> current = placements();
            if (!previous.isEmpty()) {
                for (auto item = current.cbegin(); item != current.cend(); ++item) {
                    if (!previous.contains(item.key())) {
                        const double relative = item.value().value(
                                QStringLiteral("distance")).toDouble()
                                - distance;
                        QVERIFY2(relative >= 38.0,
                                 "forest dressing popped in near the rider");
                    } else {
                        const QVariantMap prior = previous.value(item.key());
                        QCOMPARE(item.value().value(QStringLiteral("x")),
                                 prior.value(QStringLiteral("x")));
                        QCOMPARE(item.value().value(QStringLiteral("y")),
                                 prior.value(QStringLiteral("y")));
                        QCOMPARE(item.value().value(QStringLiteral("z")),
                                 prior.value(QStringLiteral("z")));
                        QCOMPARE(item.value().value(QStringLiteral("lateral")),
                                 prior.value(QStringLiteral("lateral")));
                    }
                }
                for (auto item = previous.cbegin();
                     item != previous.cend(); ++item) {
                    if (!current.contains(item.key())) {
                        const double relative = item.value().value(
                                QStringLiteral("distance")).toDouble()
                                - previousDistance;
                        QVERIFY2(relative <= -10.0,
                                 "forest dressing disappeared near the rider");
                    }
                }
            }
            previous = current;
            previousDistance = distance;
        }
    }

    void streamedVegetationKeepsVisibleDelegateIdentity()
    {
        const WorkoutGameCourse course = longFlowingMtbCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);

        WorkoutGameVisualSnapshot frame = frameAt(road, 20.0);
        frame.simulation.workoutTimeMs = 5000;
        frame.presentationTimeMs = frame.simulation.workoutTimeMs;
        viewModel.setFrame(frame, 225.0, 225.0, 88, 150, 7);

        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.resize(960, 540);
        window.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        window.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        QCoreApplication::processEvents();

        using DelegateMap = QHash<QString, QPointer<QObject>>;
        const auto delegates = [&window](const QString &objectName)
                -> DelegateMap {
            DelegateMap result;
            const QList<QObject *> objects =
                    window.rootObject()->findChildren<QObject *>(objectName);
            for (QObject *object : objects) {
                const QString id = object->property(
                        "vegetationId").toString();
                if (id.isEmpty() || result.contains(id)) return {};
                result.insert(id, object);
            }
            return result;
        };

        DelegateMap priorTrees = delegates(QStringLiteral("workoutGameTree"));
        DelegateMap priorFloor = delegates(
                QStringLiteral("workoutGameForestFloorProp"));
        DelegateMap priorVerge = delegates(
                QStringLiteral("workoutGameForestVergeCluster"));
        QVERIFY(!priorTrees.isEmpty());
        QVERIFY(!priorFloor.isEmpty());
        QVERIFY(!priorVerge.isEmpty());

        int treeBoundaries = 0;
        int dressingBoundaries = 0;
        int invisibleTreeEntries = 0;
        int invisibleDressingEntries = 0;
        double maximumNewDressingOpacity = 0.0;
        double nearestNewDressingMeters =
                std::numeric_limits<double>::infinity();
        for (int step = 1; step <= 120; ++step) {
            const double distance = 20.0 + 0.5 * step;
            frame = frameAt(road, distance);
            frame.simulation.workoutTimeMs = 5000 + step * 50;
            frame.presentationTimeMs = frame.simulation.workoutTimeMs;
            viewModel.setFrame(frame, 225.0, 225.0, 88, 150, 7);
            QCoreApplication::processEvents();

            const DelegateMap currentTrees = delegates(
                    QStringLiteral("workoutGameTree"));
            const DelegateMap currentFloor = delegates(
                    QStringLiteral("workoutGameForestFloorProp"));
            const DelegateMap currentVerge = delegates(
                    QStringLiteral("workoutGameForestVergeCluster"));
            struct OverlapResult {
                int retained = 0;
                bool alive = true;
                bool sameObject = true;
            };
            const auto inspectOverlap = [](const DelegateMap &before,
                                           const DelegateMap &after) {
                OverlapResult result;
                for (auto item = after.cbegin(); item != after.cend(); ++item) {
                    if (!before.contains(item.key())) continue;
                    ++result.retained;
                    result.alive = result.alive
                            && !before.value(item.key()).isNull();
                    result.sameObject = result.sameObject
                            && before.value(item.key()).data()
                                    == item.value().data();
                }
                return result;
            };
            const OverlapResult retainedTrees = inspectOverlap(
                    priorTrees, currentTrees);
            const OverlapResult retainedFloor = inspectOverlap(
                    priorFloor, currentFloor);
            const OverlapResult retainedVerge = inspectOverlap(
                    priorVerge, currentVerge);
            QVERIFY2(retainedTrees.alive && retainedTrees.sameObject,
                     "visible tree was recreated");
            QVERIFY2(retainedFloor.alive && retainedFloor.sameObject,
                     "visible forest-floor prop was recreated");
            QVERIFY2(retainedVerge.alive && retainedVerge.sameObject,
                     "visible verge cluster was recreated");

            const auto keySet = [](const DelegateMap &map) {
                QSet<QString> result;
                for (auto item = map.cbegin(); item != map.cend(); ++item) {
                    result.insert(item.key());
                }
                return result;
            };
            const bool modelsChanged =
                    keySet(currentTrees) != keySet(priorTrees)
                    || keySet(currentFloor) != keySet(priorFloor)
                    || keySet(currentVerge) != keySet(priorVerge);
            if (modelsChanged) {
                QVERIFY(retainedTrees.retained >= 4);
                QVERIFY(retainedFloor.retained + retainedVerge.retained >= 3);
            }
            if (keySet(currentTrees) != keySet(priorTrees)) {
                ++treeBoundaries;
                for (auto item = currentTrees.cbegin();
                     item != currentTrees.cend(); ++item) {
                    if (priorTrees.contains(item.key())) continue;
                    const double relative = item.value()->property(
                            "relativeDistance").toDouble();
                    const double opacity = item.value()->property(
                            "opacity").toDouble();
                    QVERIFY2(opacity <= 0.05,
                             qPrintable(QStringLiteral(
                                 "new tree entered at %1 m with opacity %2")
                                 .arg(relative, 0, 'f', 3)
                                 .arg(opacity, 0, 'f', 3)));
                    ++invisibleTreeEntries;
                }
            }
            const bool dressingChanged =
                    keySet(currentFloor) != keySet(priorFloor)
                    || keySet(currentVerge) != keySet(priorVerge);
            if (dressingChanged) {
                ++dressingBoundaries;
                const auto inspectNewDressing = [&](const DelegateMap &before,
                                                    const DelegateMap &after) {
                    for (auto item = after.cbegin();
                         item != after.cend(); ++item) {
                        if (before.contains(item.key())) continue;
                        const double relative = item.value()->property(
                                "relativeDistance").toDouble();
                        const double opacity = item.value()->property(
                                "opacity").toDouble();
                        maximumNewDressingOpacity = std::max(
                                maximumNewDressingOpacity, opacity);
                        nearestNewDressingMeters = std::min(
                                nearestNewDressingMeters, relative);
                        ++invisibleDressingEntries;
                    }
                };
                inspectNewDressing(priorFloor, currentFloor);
                inspectNewDressing(priorVerge, currentVerge);
            }
            priorTrees = currentTrees;
            priorFloor = currentFloor;
            priorVerge = currentVerge;
        }
        QVERIFY2(treeBoundaries >= 1 && invisibleTreeEntries >= 1,
                 "test did not cross a tree streaming boundary");
        QVERIFY2(dressingBoundaries >= 2 && invisibleDressingEntries >= 2,
                 "test did not cross enough floor-dressing boundaries");
        QVERIFY2(maximumNewDressingOpacity <= 0.05,
                 qPrintable(QStringLiteral(
                     "new floor dressing entered at %1 m with opacity %2")
                     .arg(nearestNewDressingMeters, 0, 'f', 3)
                     .arg(maximumNewDressingOpacity, 0, 'f', 3)));
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
            WorkoutGameVisualSnapshot frame = frameAt(road, distance);
            frame.simulation.workoutTimeMs = std::int64_t(std::llround(
                    1000.0 * double(frameIndex) / 30.0));
            viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
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

    void cameraRemainsSmoothThroughLongFlowingDescent()
    {
        const WorkoutGameCourse course = longFlowingMtbCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 250.0);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, 250.0);

        constexpr double FrameSeconds = 1.0 / 60.0;
        constexpr double SpeedMetersPerSecond = 8.0;
        constexpr int FrameCount = 2400;
        const double descentStart = course.sections.front().lengthMeters;
        double priorX = 0.0;
        double priorY = 0.0;
        double priorZ = 0.0;
        double priorYaw = 0.0;
        double priorYawStep = 0.0;
        double maximumStep = 0.0;
        double maximumYawStep = 0.0;
        double maximumYawAcceleration = 0.0;

        for (int frameIndex = 0; frameIndex < FrameCount; ++frameIndex) {
            const double distance = descentStart + 12.0
                    + double(frameIndex) * FrameSeconds
                        * SpeedMetersPerSecond;
            WorkoutGameVisualSnapshot frame = frameAt(road, distance);
            frame.simulation.workoutTimeMs = 5000
                    + qint64(std::llround(
                        double(frameIndex) * FrameSeconds * 1000.0));
            viewModel.setFrame(frame, 151.0, 151.0, 82, 145, 7);
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

        QVERIFY2(maximumStep <= 0.30,
                 qPrintable(QStringLiteral("camera step reached %1 m")
                         .arg(maximumStep)));
        QVERIFY2(maximumYawStep <= 0.035,
                 qPrintable(QStringLiteral("camera yaw step reached %1 rad")
                         .arg(maximumYawStep)));
        QVERIFY2(maximumYawAcceleration <= 0.012,
                 qPrintable(QStringLiteral(
                         "camera yaw acceleration reached %1 rad/frame^2")
                         .arg(maximumYawAcceleration)));
    }

    void cameraKeepsTheRiderFramedThroughTheLongCourse()
    {
        const WorkoutGameCourse course = longFlowingMtbCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 250.0);
        QVERIFY(road.ready);
        QVERIFY(road.totalLengthMeters > 3000.0);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, 250.0);

        constexpr double FrameSeconds = 1.0 / 60.0;
        constexpr double SpeedMetersPerSecond = 10.0;
        const int frameCount = int(std::ceil(
                (road.totalLengthMeters + 200.0)
                    / (SpeedMetersPerSecond * FrameSeconds)));
        double maximumHorizontalAngleDegrees = 0.0;
        double maximumVerticalAngleDegrees = 0.0;

        for (int frameIndex = 0; frameIndex <= frameCount; ++frameIndex) {
            const double requestedDistance = double(frameIndex)
                    * SpeedMetersPerSecond * FrameSeconds;
            const double distance = std::min(
                    road.totalLengthMeters, requestedDistance);
            WorkoutGameVisualSnapshot frame = frameAt(road, distance);
            frame.simulation.workoutTimeMs = qint64(std::llround(
                    double(frameIndex) * FrameSeconds * 1000.0));
            viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);

            const double viewX = viewModel.cameraTargetX()
                    - viewModel.cameraX();
            const double viewY = viewModel.cameraTargetY()
                    - viewModel.cameraY();
            const double viewZ = viewModel.cameraTargetZ()
                    - viewModel.cameraZ();
            const double riderX = viewModel.riderX() - viewModel.cameraX();
            const double riderY = viewModel.riderY() + 0.9
                    - viewModel.cameraY();
            const double riderZ = viewModel.riderZ() - viewModel.cameraZ();
            const double viewHorizontal = std::hypot(viewX, viewZ);
            const double riderHorizontal = std::hypot(riderX, riderZ);
            QVERIFY(viewHorizontal > 1.0e-6);
            QVERIFY(riderHorizontal > 1.0e-6);
            const double horizontalAngle = std::acos(std::clamp(
                    (viewX * riderX + viewZ * riderZ)
                        / (viewHorizontal * riderHorizontal),
                    -1.0, 1.0)) * 180.0 / std::acos(-1.0);
            const double verticalAngle = std::abs(
                    std::atan2(riderY, riderHorizontal)
                    - std::atan2(viewY, viewHorizontal))
                        * 180.0 / std::acos(-1.0);
            maximumHorizontalAngleDegrees = std::max(
                    maximumHorizontalAngleDegrees, horizontalAngle);
            maximumVerticalAngleDegrees = std::max(
                    maximumVerticalAngleDegrees, verticalAngle);
        }

        QVERIFY2(maximumHorizontalAngleDegrees <= 18.0,
                 qPrintable(QStringLiteral(
                     "rider left horizontal framing by %1 degrees")
                     .arg(maximumHorizontalAngleDegrees)));
        QVERIFY2(maximumVerticalAngleDegrees <= 16.0,
                 qPrintable(QStringLiteral(
                     "rider left vertical framing by %1 degrees")
                     .arg(maximumVerticalAngleDegrees)));
    }

    void cameraYawUsesElapsedTimeThroughIrregularSharpTurns()
    {
        const WorkoutGameCourse course = sharpTurningCameraCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 250.0);
        QVERIFY(road.ready);
        bool hasNearNinetyTurn = false;
        for (const WorkoutGameRoadPiece &piece : road.pieces) {
            hasNearNinetyTurn = hasNearNinetyTurn
                    || std::abs(piece.turnRadians) >= 1.30;
        }
        QVERIFY2(hasNearNinetyTurn,
                 "camera fixture does not contain a near-90-degree turn");

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, 250.0);
        constexpr std::array<int, 9> FrameIntervalsMs = {{
            16, 33, 8, 24, 50, 17, 41, 12, 29
        }};
        constexpr double SpeedMetersPerSecond = 9.0;
        constexpr double MaximumYawVelocityRadiansPerSecond = 1.12;
        constexpr double MaximumYawAccelerationRadiansPerSecondSquared = 4.5;
        std::int64_t timeMs = 5000;
        double distanceMeters = 4.0;
        double priorYaw = 0.0;
        double priorYawVelocity = 0.0;
        double priorElapsedSeconds = 0.0;
        bool havePriorYaw = false;
        bool havePriorYawVelocity = false;
        double yawTravel = 0.0;

        for (int frameIndex = 0; frameIndex < 1800; ++frameIndex) {
            const int intervalMs = FrameIntervalsMs[
                    std::size_t(frameIndex) % FrameIntervalsMs.size()];
            timeMs += intervalMs;
            distanceMeters = std::min(
                    road.totalLengthMeters - 2.0,
                    distanceMeters + SpeedMetersPerSecond
                        * double(intervalMs) / 1000.0);
            WorkoutGameVisualSnapshot frame = frameAt(road, distanceMeters);
            frame.simulation.workoutTimeMs = timeMs;
            viewModel.setFrame(frame, 225.0, 225.0, 88, 150, 7);
            const double yaw = std::atan2(
                    viewModel.cameraTargetX() - viewModel.cameraX(),
                    viewModel.cameraTargetZ() - viewModel.cameraZ());
            if (havePriorYaw) {
                const double yawStep = normalizedRadians(yaw - priorYaw);
                const double elapsedSeconds = double(intervalMs) / 1000.0;
                const double yawVelocity = yawStep / elapsedSeconds;
                yawTravel += std::abs(yawStep);
                QVERIFY2(std::abs(yawStep)
                                <= MaximumYawVelocityRadiansPerSecond
                                    * elapsedSeconds + 1.0e-9,
                         qPrintable(QStringLiteral(
                             "irregular camera yaw step reached %1 rad")
                             .arg(std::abs(yawStep))));
                QVERIFY2(std::abs(yawVelocity)
                                <= MaximumYawVelocityRadiansPerSecond + 1.0e-9,
                         qPrintable(QStringLiteral(
                             "camera yaw velocity reached %1 rad/s")
                             .arg(std::abs(yawVelocity))));
                if (havePriorYawVelocity) {
                    const double velocitySampleInterval =
                            0.5 * (priorElapsedSeconds + elapsedSeconds);
                    const double acceleration =
                            (yawVelocity - priorYawVelocity)
                                / velocitySampleInterval;
                    QVERIFY2(std::abs(acceleration)
                                    <= MaximumYawAccelerationRadiansPerSecondSquared
                                        + 1.0e-6,
                             qPrintable(QStringLiteral(
                                 "camera yaw acceleration reached %1 rad/s^2 "
                                 "at frame %2 (%3 ms after %4 ms, %5 -> %6 rad/s)")
                                 .arg(std::abs(acceleration))
                                 .arg(frameIndex).arg(intervalMs)
                                 .arg(priorElapsedSeconds * 1000.0)
                                 .arg(priorYawVelocity).arg(yawVelocity)));
                }
                priorYawVelocity = yawVelocity;
                priorElapsedSeconds = elapsedSeconds;
                havePriorYawVelocity = true;
            }
            priorYaw = yaw;
            havePriorYaw = true;

            if (frameIndex % 31 == 0) {
                const double repeatedX = viewModel.cameraX();
                const double repeatedZ = viewModel.cameraZ();
                const double repeatedTargetX = viewModel.cameraTargetX();
                const double repeatedTargetZ = viewModel.cameraTargetZ();
                WorkoutGameVisualSnapshot repeated = frameAt(
                        road, std::min(road.totalLengthMeters - 2.0,
                                      distanceMeters + 3.0));
                repeated.simulation.workoutTimeMs = timeMs;
                viewModel.setFrame(repeated, 225.0, 225.0, 88, 150, 7);
                QCOMPARE(viewModel.cameraX(), repeatedX);
                QCOMPARE(viewModel.cameraZ(), repeatedZ);
                QCOMPARE(viewModel.cameraTargetX(), repeatedTargetX);
                QCOMPARE(viewModel.cameraTargetZ(), repeatedTargetZ);
            }
        }
        QVERIFY2(yawTravel >= 1.0,
                 "camera smoothing stopped following the turning route");
    }

    void cameraDoesNotAdvanceWithoutSimulationTime()
    {
        const WorkoutGameCourse course = longFlowingMtbCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 250.0);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, 250.0);

        WorkoutGameVisualSnapshot first = frameAt(road, 30.0);
        first.simulation.workoutTimeMs = 6000;
        viewModel.setFrame(first, 190.0, 190.0, 85, 145, 7);
        const double cameraX = viewModel.cameraX();
        const double cameraY = viewModel.cameraY();
        const double cameraZ = viewModel.cameraZ();
        const double targetX = viewModel.cameraTargetX();
        const double targetY = viewModel.cameraTargetY();
        const double targetZ = viewModel.cameraTargetZ();

        WorkoutGameVisualSnapshot repeated = frameAt(road, 90.0);
        repeated.simulation.workoutTimeMs = first.simulation.workoutTimeMs;
        viewModel.setFrame(repeated, 190.0, 190.0, 85, 145, 7);
        QCOMPARE(viewModel.cameraX(), cameraX);
        QCOMPARE(viewModel.cameraY(), cameraY);
        QCOMPARE(viewModel.cameraZ(), cameraZ);
        QCOMPARE(viewModel.cameraTargetX(), targetX);
        QCOMPARE(viewModel.cameraTargetY(), targetY);
        QCOMPARE(viewModel.cameraTargetZ(), targetZ);
    }

    void cameraIntegratesAQuarterSecondRenderStallInBoundedSteps()
    {
        const WorkoutGameCourse course = sharpTurningCameraCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 250.0);
        QVERIFY(road.ready);
        const auto sharp = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &piece) {
                    return std::abs(piece.turnRadians) >= 1.30;
                });
        QVERIFY(sharp != road.pieces.end());

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, 250.0);
        WorkoutGameVisualSnapshot before = frameAt(
                road, std::max(2.0, sharp->startDistanceMeters - 8.0));
        before.simulation.workoutTimeMs = 5000;
        viewModel.setFrame(before, 225.0, 225.0, 88, 150, 7);

        WorkoutGameVisualSnapshot after = frameAt(
                road, sharp->geometryAnchorDistanceMeters);
        for (int frame = 1; frame <= 20; ++frame) {
            after.simulation.workoutTimeMs = 5000 + frame * 16;
            viewModel.setFrame(after, 225.0, 225.0, 88, 150, 7);
        }
        const double priorYaw = std::atan2(
                viewModel.cameraTargetX() - viewModel.cameraX(),
                viewModel.cameraTargetZ() - viewModel.cameraZ());

        after.simulation.workoutTimeMs += 250;
        viewModel.setFrame(after, 225.0, 225.0, 88, 150, 7);
        const double yaw = std::atan2(
                viewModel.cameraTargetX() - viewModel.cameraX(),
                viewModel.cameraTargetZ() - viewModel.cameraZ());
        const double step = std::abs(normalizedRadians(yaw - priorYaw));
        QVERIFY2(step >= 0.17,
                 qPrintable(QStringLiteral(
                     "camera did not reach the stalled-frame yaw cap; "
                     "yaw step %1 rad")
                     .arg(step)));
        QVERIFY2(step <= 0.180001,
                 qPrintable(QStringLiteral(
                     "camera stall catch-up jumped %1 rad").arg(step)));

        const double stalledYaw = yaw;
        after.simulation.workoutTimeMs += 16;
        viewModel.setFrame(after, 225.0, 225.0, 88, 150, 7);
        const double recoveredYaw = std::atan2(
                viewModel.cameraTargetX() - viewModel.cameraX(),
                viewModel.cameraTargetZ() - viewModel.cameraZ());
        const double recoveryStep = std::abs(normalizedRadians(
                recoveredYaw - stalledYaw));
        QVERIFY2(recoveryStep <= 0.018,
                 qPrintable(QStringLiteral(
                     "camera recovery frame jumped %1 rad")
                     .arg(recoveryStep)));
    }

    void longBermCourseKeepsVisibleGeometryChunkBounded()
    {
        const WorkoutGameCourse course = longFlowingMtbCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, 250.0);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, 250.0);
        const double descentDistance = course.sections.front().lengthMeters
                + 400.0;
        viewModel.setFrame(
                frameAt(road, descentDistance), 151.0, 151.0, 82, 145, 7);

        QTRY_VERIFY_WITH_TIMEOUT(qobject_cast<WorkoutGame3DGeometry *>(
                    viewModel.bermGeometry())->ready(), 3000);
        auto *geometry = qobject_cast<WorkoutGame3DGeometry *>(
                viewModel.bermGeometry());
        QVERIFY(geometry);
        QVERIFY2(geometry->sampleCount() < 1600,
                 "the entire course berm mesh is still resident");
        QVERIFY2(geometry->triangleCount() < 18000,
                 "visible berm geometry exceeds the frame budget");
    }

    void exportsConfiguredCourseAuditFrames()
    {
        const QString documentPath = qEnvironmentVariable(
                "GC_WORKOUT_GAME_3D_COURSE_DOCUMENT");
        const QString outputDirectory = qEnvironmentVariable(
                "GC_WORKOUT_GAME_3D_COURSE_AUDIT_DIR");
        if (documentPath.isEmpty() || outputDirectory.isEmpty()) {
            QSKIP("Set the course document and audit directory to export frames");
        }
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }

        QFile file(documentPath);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
        WorkoutGameCourseDocument document;
        QCOMPARE(WorkoutGameCourseDocumentCodec::decode(
                         file.readAll(), document),
                 WorkoutGameCourseDocumentStatus::Ready);
        const WorkoutGameCourse course =
                WorkoutGameDistancePlayback::visualCourse(document.course);
        QCOMPARE(course.status, WorkoutGameCourseStatus::Ready);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, document.ftpWatts);
        QVERIFY(road.ready);
        QVERIFY(QDir().mkpath(outputDirectory));

        std::size_t longestClimb = course.sections.size();
        std::size_t longestRecovery = course.sections.size();
        double climbLength = 0.0;
        double recoveryLength = 0.0;
        for (std::size_t index = 0; index < course.sections.size(); ++index) {
            const WorkoutGameSection &section = course.sections[index];
            if (section.terrain == WorkoutGameTerrainKind::Climb
                    && section.lengthMeters > climbLength) {
                longestClimb = index;
                climbLength = section.lengthMeters;
            }
            const bool recovery = section.feature
                        == WorkoutGameFeature::RecoveryDescent
                    || section.feature == WorkoutGameFeature::CooldownDescent;
            if (recovery && section.lengthMeters > recoveryLength) {
                longestRecovery = index;
                recoveryLength = section.lengthMeters;
            }
        }
        QVERIFY2(longestClimb < road.timeline.size(),
                 "configured course lacks a climbing audit section");
        QVERIFY2(longestRecovery < road.timeline.size(),
                 "configured course lacks a recovery audit section");
        const WorkoutGameRoadTimelineSection &climb =
                road.timeline[longestClimb];
        const WorkoutGameRoadTimelineSection &recovery =
                road.timeline[longestRecovery];
        const double recoverySpan = recovery.endDistanceMeters
                - recovery.startDistanceMeters;
        std::vector<double> distances = {
            std::min(60.0, road.totalLengthMeters * 0.1),
            (climb.startDistanceMeters + climb.endDistanceMeters) * 0.5,
            recovery.startDistanceMeters + recoverySpan * 0.08,
            recovery.startDistanceMeters + recoverySpan * 0.25,
            recovery.startDistanceMeters + recoverySpan * 0.50,
            recovery.startDistanceMeters + recoverySpan * 0.78
        };
        for (int index = 1; index <= 12; ++index) {
            distances.push_back(road.totalLengthMeters
                    * double(index) / 13.0);
        }
        std::sort(distances.begin(), distances.end());
        distances.erase(std::unique(
                distances.begin(), distances.end(),
                [](double first, double second) {
                    return std::abs(first - second) < 0.01;
                }), distances.end());
        const QString requestedDistances = qEnvironmentVariable(
                "GC_WORKOUT_GAME_3D_COURSE_AUDIT_DISTANCES");
        if (!requestedDistances.isEmpty()) {
            distances.clear();
            const QStringList values = requestedDistances.split(
                    QLatin1Char(','), Qt::SkipEmptyParts);
            for (const QString &value : values) {
                bool ok = false;
                const double distance = value.trimmed().toDouble(&ok);
                QVERIFY2(ok && distance >= 0.0
                                && distance <= road.totalLengthMeters,
                         qPrintable(QStringLiteral(
                             "invalid audit distance: %1").arg(value)));
                distances.push_back(distance);
            }
            QVERIFY(!distances.empty());
        }

        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.resize(1280, 720);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        window.setCourse(course, document.ftpWatts);
        auto *auditViewModel = qobject_cast<WorkoutGame3DViewModel *>(
                window.rootContext()->contextProperty(
                    QStringLiteral("workoutGame3D")).value<QObject *>());
        QObject *bermModel = window.rootObject()->findChild<QObject *>(
                QStringLiteral("bermGeometryModel"));
        QVERIFY(auditViewModel);
        QVERIFY(bermModel);
        WorkoutGameVisualSnapshot opening = frameAt(road, distances.front());
        opening.simulation.workoutTimeMs = 0;
        opening.presentationTimeMs = 0;
        window.setFrame(opening, 220.0, 220.0, 86, 148, 7);
        for (std::size_t index = 0; index < distances.size(); ++index) {
            WorkoutGameVisualSnapshot frame = frameAt(road, distances[index]);
            frame.simulation.workoutTimeMs = 5000
                    + qint64(index) * 1000;
            frame.presentationTimeMs = frame.simulation.workoutTimeMs;
            window.setFrame(frame, 220.0, 220.0, 86, 148, 7);
            QCOMPARE(auditViewModel->cameraPresentation(),
                     QStringLiteral("chase"));
            QCOMPARE(auditViewModel->cameraPresentationBlend(), 0.0);
            QTest::qWait(index == 0u ? 700 : 350);
            frame.simulation.workoutTimeMs += 16;
            window.setFrame(frame, 220.0, 220.0, 86, 148, 7);
            QTest::qWait(150);
            QCOMPARE(bermModel->property("geometry").value<QObject *>(),
                     auditViewModel->bermGeometry());
            if (frame.world.terrain == WorkoutGameTerrainKind::Berm) {
                auto *geometry = qobject_cast<WorkoutGame3DGeometry *>(
                        auditViewModel->bermGeometry());
                QVERIFY(geometry);
                QVERIFY(geometry->ready());
                QVERIFY(geometry->sampleCount() > 0);
            }
            const QImage rendered = window.grabWindow();
            QVERIFY(!rendered.isNull());
            QVERIFY2(sampledColorCount(rendered) > 35,
                     "configured course scene appears blank");
            const QString output = QDir(outputDirectory).filePath(
                    QStringLiteral("course-%1-%2m.png")
                        .arg(index + 1, 2, 10, QLatin1Char('0'))
                        .arg(qRound(distances[index])));
            QVERIFY2(rendered.save(output), qPrintable(output));
            const QRect riderRegion(
                    0,
                    rendered.height() * 2 / 5,
                    rendered.width(),
                    rendered.height() * 3 / 5);
            const int bluePixels = riderBluePixels(rendered, riderRegion);
            QVERIFY2(bluePixels > 20,
                     qPrintable(QStringLiteral(
                         "rider body has only %1 blue pixels at %2 m")
                         .arg(bluePixels).arg(distances[index])));
            const QVariantList riderPoints = window.rootObject()->property(
                    "riderWheelFrustumScreenPoints").toList();
            QVERIFY(riderPoints.size() >= 17);
            for (const QVariant &pointValue : riderPoints) {
                const QVector3D point = pointValue.value<QVector3D>();
                QVERIFY(std::isfinite(point.x()));
                QVERIFY(std::isfinite(point.y()));
                QVERIFY2(point.x() >= 2.0f
                                && point.x() <= rendered.width() - 2.0f
                                && point.y() >= 2.0f
                                && point.y() <= rendered.height() - 2.0f,
                         qPrintable(QStringLiteral(
                             "rider bound projected outside frame at %1 m: "
                             "(%2, %3)")
                             .arg(distances[index])
                             .arg(point.x()).arg(point.y())));
            }
        }
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
        WorkoutGameVisualSnapshot openingFrame = frame;
        openingFrame.simulation.workoutTimeMs = 0;
        openingFrame.presentationTimeMs = 0;
        viewModel.setFrame(openingFrame, 235.0, 220.0, 92, 152, 7);
        frame.simulation.workoutTimeMs = 5000;
        frame.presentationTimeMs = 5000;
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
        const double bodyY = body->property("y").toDouble();
        QVERIFY(bodyY >= 1.13);
        QVERIFY(bodyY <= 1.153);
        QVERIFY(viewModel.rearSuspensionCompression() > 0.70);
        QVERIFY(viewModel.frontSuspensionCompression() > 0.70);
        QObject *sprungBike = window.rootObject()->findChild<QObject *>(
                QStringLiteral("sprungBikeNode"));
        QVERIFY(sprungBike);
        QVERIFY(sprungBike->property("y").toDouble() < -0.07);

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
        QObject *leftPedal = window.rootObject()->findChild<QObject *>(
                QStringLiteral("leftPedalContact"));
        QObject *rightPedal = window.rootObject()->findChild<QObject *>(
                QStringLiteral("rightPedalContact"));
        QObject *riderTexture = window.rootObject()->findChild<QObject *>(
                QStringLiteral("riderPixelTexture"));
        QVERIFY(rearWheel);
        QVERIFY(frontWheel);
        QVERIFY(crank);
        QVERIFY(lowerLeg);
        QVERIFY(leftPedal);
        QVERIFY(rightPedal);
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
        const QVector3D firstLeftPedal =
                leftPedal->property("position").value<QVector3D>();
        const QVector3D firstRightPedal =
                rightPedal->property("position").value<QVector3D>();
        QVERIFY(std::abs(firstLeftPedal.x() + 0.13f) < 0.001f);
        QVERIFY(std::abs(firstRightPedal.x() - 0.13f) < 0.001f);
        QVERIFY((firstLeftPedal - firstRightPedal).length() > 0.28f);

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
        const QVector3D secondLeftPedal =
                leftPedal->property("position").value<QVector3D>();
        const QVector3D secondRightPedal =
                rightPedal->property("position").value<QVector3D>();
        QVERIFY(std::abs(secondRear.x() - firstRear.x()) > 1.0);
        QCOMPARE(secondRear.x(), secondFront.x());
        QCOMPARE(firstRear.x(), firstFront.x());
        QVERIFY(std::abs(secondCrank.x() - firstCrank.x()) > 1.0);
        QVERIFY((secondLeg - firstLeg).length() > 0.5f);
        QVERIFY((secondLeftPedal - firstLeftPedal).length() > 0.01f);
        QVERIFY((secondRightPedal - firstRightPedal).length() > 0.01f);
        QVERIFY(std::abs((secondLeftPedal.y() + secondRightPedal.y())
                         - 2.0f * 0.3775f) < 0.001f);
        QVERIFY(std::abs(secondLeftPedal.z() + secondRightPedal.z())
                < 0.001f);
    }

    void riderReadabilitySourceUsesScopedLightingWithoutEmission()
    {
        QFile sceneSource(QStringLiteral(":/qml/WorkoutGame3D.qml"));
        QVERIFY(sceneSource.open(QIODevice::ReadOnly));
        const QByteArray sceneQml = sceneSource.readAll();
        QVERIFY(sceneQml.contains(
                "objectName: \"riderReadabilityLight\""));
        QVERIFY(sceneQml.contains("scope: rider"));

        QFile riderSource(QStringLiteral(":/qml/WorkoutGameRiderBike.qml"));
        QVERIFY(riderSource.open(QIODevice::ReadOnly));
        const QByteArray riderQml = riderSource.readAll();
        QVERIFY(!riderQml.contains("emissiveFactor"));
        QVERIFY(!riderQml.contains("emissiveMap"));
    }

    void riderExportsCompleteFrustumBounds()
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

        QObject *rider = window.rootObject()->findChild<QObject *>(
                QStringLiteral("riderNode"));
        QVERIFY(rider);
        QVariant result;
        QVERIFY(QMetaObject::invokeMethod(
                rider, "riderFrustumScenePoints",
                Q_RETURN_ARG(QVariant, result)));
        const QVariantList points = result.toList();
        QVERIFY2(points.size() >= 17,
                 "complete rider bounds must cover wheels, bars, torso and helmet");
        QVariant compatibilityResult;
        QVERIFY(QMetaObject::invokeMethod(
                rider, "wheelFrustumScenePoints",
                Q_RETURN_ARG(QVariant, compatibilityResult)));
        QCOMPARE(compatibilityResult.toList().size(), points.size());

        float minimumX = std::numeric_limits<float>::max();
        float maximumX = std::numeric_limits<float>::lowest();
        float minimumY = std::numeric_limits<float>::max();
        float maximumY = std::numeric_limits<float>::lowest();
        float minimumZ = std::numeric_limits<float>::max();
        float maximumZ = std::numeric_limits<float>::lowest();
        for (const QVariant &pointValue : points) {
            const QVector3D point = pointValue.value<QVector3D>();
            QVERIFY(std::isfinite(point.x()));
            QVERIFY(std::isfinite(point.y()));
            QVERIFY(std::isfinite(point.z()));
            minimumX = std::min(minimumX, point.x());
            maximumX = std::max(maximumX, point.x());
            minimumY = std::min(minimumY, point.y());
            maximumY = std::max(maximumY, point.y());
            minimumZ = std::min(minimumZ, point.z());
            maximumZ = std::max(maximumZ, point.z());
        }
        QVERIFY(maximumX - minimumX >= 0.60f);
        QVERIFY(maximumY - minimumY >= 1.70f);
        QVERIFY(maximumZ - minimumZ >= 1.65f);
    }

    void riderReadabilityMaterialsAndScopedLightInstantiate()
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

        QObject *rider = window.rootObject()->findChild<QObject *>(
                QStringLiteral("riderNode"));
        QObject *readabilityLight = window.rootObject()->findChild<QObject *>(
                QStringLiteral("riderReadabilityLight"));
        QVERIFY(rider);
        QVERIFY(readabilityLight);
        QCOMPARE(readabilityLight->property("scope").value<QObject *>(), rider);
        QCOMPARE(readabilityLight->property("castsShadow").toBool(), false);
        const double brightness =
                readabilityLight->property("brightness").toDouble();
        QVERIFY(brightness >= 0.5);
        QVERIFY(brightness <= 0.75);

        const std::array<std::pair<const char *, QColor>, 4> materials = {{
            {"riderTireMaterial", QColor(QStringLiteral("#252c2e"))},
            {"riderComponentMaterial", QColor(QStringLiteral("#293235"))},
            {"riderShortsMaterial", QColor(QStringLiteral("#303a3e"))},
            {"riderDarkMaterial", QColor(QStringLiteral("#2b3437"))}
        }};
        for (const auto &[name, expectedColor] : materials) {
            QObject *material = window.rootObject()->findChild<QObject *>(
                    QString::fromLatin1(name));
            QVERIFY2(material, name);
            const QColor color = material->property("baseColor").value<QColor>();
            QCOMPARE(color, expectedColor);
            QVERIFY(color.lightnessF() < 0.30);
            QVERIFY(color.lightnessF() > 0.12);
        }
    }

    void coastPoseKeepsFeetOnRenderedPedalsFromEveryCrankPhase()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::SmoothTrail);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);

        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.resize(960, 540);
        window.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        window.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        QObject *rider = window.rootObject()->findChild<QObject *>(
                QStringLiteral("riderNode"));
        QObject *crank = window.rootObject()->findChild<QObject *>(
                QStringLiteral("crankPivot"));
        QObject *leftPedal = window.rootObject()->findChild<QObject *>(
                QStringLiteral("leftPedalContact"));
        QObject *rightPedal = window.rootObject()->findChild<QObject *>(
                QStringLiteral("rightPedalContact"));
        QVERIFY(rider);
        QVERIFY(crank);
        QVERIFY(leftPedal);
        QVERIFY(rightPedal);

        for (double cycles : {0.0, 0.125, 0.50, 0.875}) {
            WorkoutGameVisualSnapshot frame = frameAt(road, 8.0);
            frame.riderPedalCycles = cycles;
            viewModel.setFrame(frame, 190.0, 190.0, 86, 148, 6);
            QTest::qWait(140);
            viewModel.setFrame(frame, 0.0, 190.0, 0, 148, 6);
            QTest::qWait(140);

            QVERIFY(std::abs(rider->property(
                        "crankRenderAngle").toDouble() - 90.0) < 0.5);
            const QVector3D crankRotation =
                    crank->property("eulerRotation").value<QVector3D>();
            QVERIFY(std::abs(crankRotation.x() - 90.0f) < 0.5f);
            const QVector3D left =
                    leftPedal->property("position").value<QVector3D>();
            const QVector3D right =
                    rightPedal->property("position").value<QVector3D>();
            QVERIFY(std::abs(left.y() - 0.3775f) < 0.002f);
            QVERIFY(std::abs(left.z() - 0.16f) < 0.002f);
            QVERIFY(std::abs(right.y() - 0.3775f) < 0.002f);
            QVERIFY(std::abs(right.z() + 0.16f) < 0.002f);
        }
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
                    return candidate.terrain == WorkoutGameTerrainKind::Berm;
                });
        QVERIFY(piece != road.pieces.end());
        QVERIFY(!piece->challenge.enabled);
        const double distance = piece->geometryAnchorDistanceMeters;
        const WorkoutGameBermGeometryProfile profile =
                WorkoutGameBermGeometry::profile(piece->difficulty);
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
        frame.feature.bermLineBias = 0.0;
        viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
        QCOMPARE(viewModel.terrainName(), QStringLiteral("Singletrack"));
        QVERIFY(!viewModel.featureHudVisible());
        const double centerRoll = viewModel.riderRoll();
        const double centerY = viewModel.riderY();
        QVERIFY(std::abs(centerRoll) > 15.0);
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

        frame.feature.bermLineBias = 1.0;
        frame.feature.lateralOffsetMeters = profile.effortLineLateralMeters(
                0.0, piece->turnRadians, 1.0);
        for (int frameIndex = 0; frameIndex < 30; ++frameIndex) {
            frame.simulation.workoutTimeMs += 20;
            viewModel.setFrame(frame, 330.0, 220.0, 88, 150, 9);
        }
        const double highRoll = viewModel.riderRoll();
        const double highY = viewModel.riderY();
        QVERIFY(std::abs(highRoll) > std::abs(centerRoll));
        QVERIFY(highY > centerY + 0.10);

        frame.feature.bermLineBias = -1.0;
        frame.feature.lateralOffsetMeters = profile.effortLineLateralMeters(
                0.0, piece->turnRadians, -1.0);
        for (int frameIndex = 0; frameIndex < 30; ++frameIndex) {
            frame.simulation.workoutTimeMs += 20;
            viewModel.setFrame(frame, 110.0, 220.0, 72, 145, 4);
        }
        QVERIFY(std::abs(viewModel.riderRoll()) < std::abs(centerRoll));
        QVERIFY(viewModel.riderY() < centerY - 0.10);
        QVERIFY2(riderCameraAngleDegrees() < 10.0,
                 qPrintable(QStringLiteral("safe-line camera offset %1 degrees")
                     .arg(riderCameraAngleDegrees())));
    }

    void ordinaryBankedTurnDrivesRiderLineHeightAndRoll()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 0x423107u;
        course.durationMs = 90000;
        WorkoutGameSection trail;
        trail.feature = WorkoutGameFeature::Trail;
        trail.terrain = WorkoutGameTerrainKind::SmoothTrail;
        trail.durationMs = course.durationMs;
        trail.lengthMeters = 360.0;
        trail.targetWatts = 190.0;
        trail.difficulty = 0.7;
        trail.challengeCount = 0;
        course.sections = {trail};
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.bank.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        const double distance = piece->geometryAnchorDistanceMeters;

        WorkoutGameFeatureRuntime runtime;
        QVERIFY(runtime.configure(road));
        WorkoutGameVisualSnapshot frame = frameAt(road, distance);
        frame.simulation.activeSection = 0;
        frame.simulation.sectionProgress = distance / road.totalLengthMeters;
        frame.simulation.speedKph = 24.0;
        frame.feature = runtime.update(
                frame.simulation, 190.0, 190.0);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(frame, 190.0, 190.0, 86, 148, 6);
        const double centreY = viewModel.riderY();
        const double centreRoll = viewModel.riderRoll();
        QVERIFY(std::abs(centreRoll) > 1.0);
        QVERIFY(!viewModel.featureHudVisible());

        frame.feature = runtime.update(
                frame.simulation, 280.0, 190.0);
        for (int index = 0; index < 30; ++index) {
            frame.simulation.workoutTimeMs += 20;
            frame.feature = runtime.update(
                    frame.simulation, 280.0, 190.0);
            viewModel.setFrame(frame, 280.0, 190.0, 94, 152, 9);
        }
        const WorkoutGameRoadSample centre =
                WorkoutGameRoadCourseBuilder::sample(road, distance);
        QVERIFY(std::hypot(viewModel.riderX() - centre.center.xMeters,
                           viewModel.riderZ() - centre.center.zMeters) > 0.05);
        QVERIFY(std::abs(viewModel.riderY() - centreY) > 0.05);
        QVERIFY(std::abs(viewModel.riderRoll()) > std::abs(centreRoll));
    }

    void bermRollUsesElapsedTimeInsteadOfPresentedFrameCount()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Berm);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.terrain == WorkoutGameTerrainKind::Berm;
                });
        QVERIFY(piece != road.pieces.end());
        const WorkoutGameBermGeometryProfile profile =
                WorkoutGameBermGeometry::profile(piece->difficulty);
        const auto rollAfter = [&](int frameIntervalMs) {
            WorkoutGame3DViewModel viewModel;
            viewModel.setCourse(course, FtpWatts);
            WorkoutGameVisualSnapshot frame = frameAt(
                    road, piece->geometryAnchorDistanceMeters
                        + profile.startMeters);
            frame.world.terrain = WorkoutGameTerrainKind::Berm;
            frame.simulation.workoutTimeMs = 1000;
            frame.simulation.speedKph = 25.0;
            frame.feature.route = WorkoutGameRoute::MainLine;
            frame.feature.bermLineBias = -1.0;
            viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);

            const int frames = 400 / frameIntervalMs;
            frame = frameAt(road, piece->geometryAnchorDistanceMeters);
            frame.world.terrain = WorkoutGameTerrainKind::Berm;
            frame.simulation.speedKph = 25.0;
            frame.feature.route = WorkoutGameRoute::MainLine;
            frame.feature.bermLineBias = 1.0;
            double priorRoll = viewModel.riderRoll();
            bool bounded = true;
            for (int index = 1; index <= frames; ++index) {
                frame.simulation.workoutTimeMs =
                        1000 + index * frameIntervalMs;
                viewModel.setFrame(frame, 220.0, 220.0, 88, 150, 7);
                const double maximumStep = 75.0
                        * double(frameIntervalMs) / 1000.0;
                bounded = bounded
                        && std::abs(viewModel.riderRoll() - priorRoll)
                            <= maximumStep + 1.0e-6;
                priorRoll = viewModel.riderRoll();
            }
            return std::pair<double, bool>{viewModel.riderRoll(), bounded};
        };

        const auto twentyMillisecond = rollAfter(20);
        const auto fortyMillisecond = rollAfter(40);
        QVERIFY(twentyMillisecond.second);
        QVERIFY(fortyMillisecond.second);
        QVERIFY(std::abs(twentyMillisecond.first - fortyMillisecond.first)
                < 0.25);
    }

    void bankRollReturnsToStraightTrailWithoutAFrameJump()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 0x42310bu;
        course.durationMs = 90000;
        WorkoutGameSection trail;
        trail.feature = WorkoutGameFeature::Trail;
        trail.terrain = WorkoutGameTerrainKind::SmoothTrail;
        trail.durationMs = course.durationMs;
        trail.lengthMeters = 360.0;
        trail.targetWatts = 190.0;
        trail.difficulty = 0.7;
        trail.challengeCount = 0;
        course.sections = {trail};
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        auto piece = road.pieces.begin();
        while (piece != road.pieces.end()) {
            const auto next = std::next(piece);
            if (piece->bank.enabled && next != road.pieces.end()
                    && !next->bank.enabled) {
                break;
            }
            ++piece;
        }
        QVERIFY(piece != road.pieces.end());

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        WorkoutGameVisualSnapshot frame = frameAt(
                road, piece->geometryAnchorDistanceMeters);
        frame.simulation.speedKph = 28.0;
        frame.feature.route = WorkoutGameRoute::MainLine;
        frame.feature.bermLineBias = 1.0;
        frame.simulation.workoutTimeMs = 1000;
        viewModel.setFrame(frame, 280.0, 190.0, 94, 152, 9);
        for (int index = 1; index <= 30; ++index) {
            frame.simulation.workoutTimeMs = 1000 + index * 20;
            viewModel.setFrame(frame, 280.0, 190.0, 94, 152, 9);
        }
        const double bankRoll = viewModel.riderRoll();
        QVERIFY(std::abs(bankRoll) > 1.0);

        frame = frameAt(road,
                piece->startDistanceMeters + piece->lengthMeters + 0.01);
        frame.world.rider.rollDegrees = 0.0;
        frame.simulation.speedKph = 28.0;
        frame.simulation.workoutTimeMs = 1608;
        viewModel.setFrame(frame, 190.0, 190.0, 86, 148, 6);
        QVERIFY(std::abs(viewModel.riderRoll() - bankRoll)
                <= 75.0 * 0.008 + 1.0e-6);
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

    void gapJumpHudNamesAdaptiveLineAndPredictedSpeed()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::GapJump);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);

        WorkoutGameVisualSnapshot frame = frameAt(road, 12.0);
        frame.feature.ready = true;
        frame.feature.terrain = WorkoutGameTerrainKind::GapJump;
        frame.feature.phase = WorkoutGameFeaturePhase::Measure;
        frame.feature.launchWindowActive = true;
        frame.feature.launchPowerHoldMilliseconds = 250;
        frame.feature.provisionalGapLine = WorkoutGameGapJumpLine::Long;
        frame.feature.predictedApproachSpeedMetersPerSecond = 7.9;
        viewModel.setFrame(frame, 225.0, 220.0, 88, 149, 8);

        QCOMPARE(viewModel.featureName(),
                 QStringLiteral("Gap jump - LONG - 28.4 KM/H"));
        QCOMPARE(viewModel.featureActionText(), QStringLiteral("Accelerate"));
        QCOMPARE(viewModel.powerReadinessPercent(), 50);

        frame.feature.phase = WorkoutGameFeaturePhase::Committed;
        frame.feature.gapLineLocked = true;
        frame.feature.lockedGapLine = WorkoutGameGapJumpLine::Medium;
        frame.feature.predictedApproachSpeedMetersPerSecond = 5.2;
        viewModel.setFrame(frame, 225.0, 220.0, 88, 149, 8);
        QCOMPARE(viewModel.featureName(),
                 QStringLiteral("Gap jump - MEDIUM - 18.7 KM/H"));

        frame.feature.route = WorkoutGameRoute::SafeBypass;
        frame.feature.lockedGapLine = WorkoutGameGapJumpLine::None;
        viewModel.setFrame(frame, 120.0, 220.0, 70, 140, 3);
        QCOMPARE(viewModel.featureName(),
                 QStringLiteral("Gap jump - SAFE LINE"));

        frame.feature.phase = WorkoutGameFeaturePhase::Measure;
        frame.feature.gapLineLocked = false;
        frame.feature.route = WorkoutGameRoute::MainLine;
        frame.feature.provisionalGapLine = WorkoutGameGapJumpLine::None;
        frame.feature.launchWindowActive = false;
        viewModel.setFrame(frame, 120.0, 220.0, 70, 140, 3);
        QCOMPARE(viewModel.featureName(),
                 QStringLiteral("Gap jump - BUILD SPEED"));
        QCOMPARE(viewModel.featureActionText(), QStringLiteral("Preview"));
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

    void featureHudPrewarmsBeforeItsFirstApproachFrame()
    {
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(sampleCourse(), FtpWatts);
        QVERIFY(!viewModel.featureHudVisible());

        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        window.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);

        auto *hud = window.rootObject()->findChild<QQuickItem *>(
                QStringLiteral("featureHud"));
        QVERIFY(hud);
        QVERIFY(!hud->isVisible());

        window.rootObject()->setProperty("rendererPrewarming", true);
        QTRY_VERIFY(hud->isVisible());
        QVERIFY(hud->opacity() > 0.0);
        QVERIFY(hud->opacity() <= 0.01);

        window.rootObject()->setProperty("rendererPrewarming", false);
        QTRY_VERIFY(!hud->isVisible());
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
        QCOMPARE(mesh.size(), qint64(3660));
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

    void bypassRiderHeadingFollowsTheSelectedBranchTangent()
    {
        constexpr double Pi = 3.14159265358979323846;
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

        const double start = piece->challenge.bypassStartDistanceMeters;
        const double end = piece->challenge.bypassEndDistanceMeters;
        const double span = end - start;
        QVERIFY(span > 1.0);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);

        const auto branchPosition = [&](double distance) {
            const WorkoutGameRoadSample sample =
                    WorkoutGameRoadCourseBuilder::sample(road, distance);
            const double lateral = WorkoutGameTrailBranch::lateralAt(
                    distance, start, end,
                    piece->challenge.bypassLateralMeters);
            const double rightX = std::cos(sample.center.headingRadians);
            const double rightZ = -std::sin(sample.center.headingRadians);
            return std::pair<double, double>(
                    sample.center.xMeters + lateral * rightX,
                    sample.center.zMeters + lateral * rightZ);
        };

        double previousYaw = 0.0;
        bool havePreviousYaw = false;
        for (double progress : {0.08, 0.20, 0.35, 0.50,
                                0.65, 0.80, 0.92}) {
            const double distance = start + span * progress;
            WorkoutGameVisualSnapshot frame = frameAt(road, distance);
            frame.feature.ready = true;
            frame.feature.route = WorkoutGameRoute::SafeBypass;
            frame.feature.outcome = WorkoutGameFeatureOutcome::Bypassed;
            frame.feature.lateralOffsetMeters =
                    WorkoutGameTrailBranch::lateralAt(
                        distance, start, end,
                        piece->challenge.bypassLateralMeters);
            viewModel.setFrame(frame, 150.0, 220.0, 72, 145, 4);

            constexpr double DeltaMeters = 0.01;
            const auto before = branchPosition(distance - DeltaMeters);
            const auto after = branchPosition(distance + DeltaMeters);
            const double expectedYaw = std::atan2(
                    after.first - before.first,
                    after.second - before.second) * 180.0 / Pi;
            const double yawError = normalizedRadians(
                    (viewModel.riderYaw() - expectedYaw) * Pi / 180.0)
                    * 180.0 / Pi;
            QVERIFY2(std::abs(yawError) < 0.25,
                     qPrintable(QStringLiteral(
                         "bypass yaw differs from branch tangent by %1 degrees")
                         .arg(yawError)));
            if (havePreviousYaw) {
                const double step = normalizedRadians(
                        (viewModel.riderYaw() - previousYaw) * Pi / 180.0)
                        * 180.0 / Pi;
                QVERIFY2(std::abs(step) < 35.0,
                         "bypass heading changed discontinuously");
            }
            previousYaw = viewModel.riderYaw();
            havePreviousYaw = true;
        }

        const double mainDistance = start + span * 0.35;
        WorkoutGameVisualSnapshot mainFrame = frameAt(road, mainDistance);
        mainFrame.feature.ready = true;
        mainFrame.feature.route = WorkoutGameRoute::MainLine;
        viewModel.setFrame(mainFrame, 220.0, 220.0, 86, 145, 6);
        const WorkoutGameRoadSample mainSample =
                WorkoutGameRoadCourseBuilder::sample(road, mainDistance);
        const double mainYawError = normalizedRadians(
                (viewModel.riderYaw()
                 - mainSample.center.headingRadians * 180.0 / Pi)
                * Pi / 180.0) * 180.0 / Pi;
        QVERIFY(std::abs(mainYawError) < 0.01);
    }

    void technicalBypassRiderHeadingFollowsProfileTangent_data()
    {
        QTest::addColumn<int>("terrain");
        QTest::newRow("roots") << int(WorkoutGameTerrainKind::Roots);
        QTest::newRow("rock-garden")
                << int(WorkoutGameTerrainKind::RockGarden);
        QTest::newRow("rock-slab")
                << int(WorkoutGameTerrainKind::RockSlab);
    }

    void technicalBypassRiderHeadingFollowsProfileTangent()
    {
        constexpr double Pi = 3.14159265358979323846;
        QFETCH(int, terrain);
        const WorkoutGameTerrainKind kind = WorkoutGameTerrainKind(terrain);
        const WorkoutGameCourse course = catalogCourse(kind);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [kind](const WorkoutGameRoadPiece &candidate) {
                    return candidate.terrain == kind
                            && candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());

        const auto lateralAt = [&](double distance) {
            const double local = distance
                    - piece->challenge.obstacleDistanceMeters;
            switch (kind) {
            case WorkoutGameTerrainKind::Roots:
                return WorkoutGameRootGeometry::profile(
                        piece->difficulty).safeLineOffsetMeters(local);
            case WorkoutGameTerrainKind::RockGarden:
                return WorkoutGameRockGardenGeometry::profile(
                        piece->difficulty).safeLineOffsetMeters(local);
            case WorkoutGameTerrainKind::RockSlab:
                return WorkoutGameRockSlabGeometry::profile(
                        piece->difficulty).safeLineOffsetMeters(local);
            default:
                return 0.0;
            }
        };
        double transitionStart = -5.0;
        double transitionEnd = WorkoutGameRootGeometry::profile(
                piece->difficulty).activeStartMeters;
        if (kind == WorkoutGameTerrainKind::RockGarden) {
            transitionStart = -6.0;
            transitionEnd = WorkoutGameRockGardenGeometry::profile(
                    piece->difficulty).activeStartMeters;
        } else if (kind == WorkoutGameTerrainKind::RockSlab) {
            const WorkoutGameRockSlabGeometryProfile profile =
                    WorkoutGameRockSlabGeometry::profile(piece->difficulty);
            transitionStart = profile.startMeters;
            transitionEnd = profile.activeStartMeters;
        }
        const double distance = piece->challenge.obstacleDistanceMeters
                + (transitionStart + transitionEnd) * 0.5;
        const double lateral = lateralAt(distance);
        QVERIFY(std::abs(lateral) > 0.05);

        WorkoutGameVisualSnapshot frame = frameAt(road, distance);
        frame.feature.ready = true;
        frame.feature.route = WorkoutGameRoute::SafeBypass;
        frame.feature.outcome = WorkoutGameFeatureOutcome::Bypassed;
        frame.feature.lateralOffsetMeters = lateral;
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        viewModel.setFrame(frame, 150.0, 220.0, 72, 145, 4);

        const auto branchPosition = [&](double atDistance) {
            const WorkoutGameRoadSample sample =
                    WorkoutGameRoadCourseBuilder::sample(road, atDistance);
            const double offset = lateralAt(atDistance);
            return std::pair<double, double>(
                    sample.center.xMeters
                        + offset * std::cos(sample.center.headingRadians),
                    sample.center.zMeters
                        - offset * std::sin(sample.center.headingRadians));
        };
        constexpr double DeltaMeters = 0.01;
        const auto before = branchPosition(distance - DeltaMeters);
        const auto after = branchPosition(distance + DeltaMeters);
        const double expectedYaw = std::atan2(
                after.first - before.first,
                after.second - before.second) * 180.0 / Pi;
        const double yawError = normalizedRadians(
                (viewModel.riderYaw() - expectedYaw) * Pi / 180.0)
                * 180.0 / Pi;
        QVERIFY2(std::abs(yawError) < 0.25,
                 qPrintable(QStringLiteral(
                     "technical bypass yaw differs by %1 degrees")
                     .arg(yawError)));
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

    void rendersPackagedConiferGroveCatalog()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.setSource(QUrl(QStringLiteral(
                "qrc:/qml/assets/ConiferAssetHarness.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        window.resize(1280, 720);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        QTest::qWait(500);

        const QString auditDirectory = qEnvironmentVariable(
                "GC_WORKOUT_GAME_CONIFER_ASSET_AUDIT_DIR");
        if (!auditDirectory.isEmpty()) {
            QVERIFY(QDir().mkpath(auditDirectory));
        }
        const std::array<QString, 3> angleNames = {{
            QStringLiteral("front"),
            QStringLiteral("left-three-quarter"),
            QStringLiteral("rear-three-quarter")
        }};
        std::array<QImage, 3> renderedAngles;
        for (std::size_t index = 0; index < renderedAngles.size(); ++index) {
            QVERIFY(window.rootObject()->setProperty(
                    "cameraAngle", int(index)));
            QTest::qWait(250);
            renderedAngles[index] = window.grabWindow();
            QVERIFY(!renderedAngles[index].isNull());
            QCOMPARE(renderedAngles[index].size(), QSize(1280, 720));
            if (!auditDirectory.isEmpty()) {
                const QString path = QDir(auditDirectory).filePath(
                        angleNames[index] + QStringLiteral(".png"));
                QVERIFY2(renderedAngles[index].save(path), qPrintable(path));
            }
            QVERIFY2(sampledColorCount(renderedAngles[index]) > 12,
                     qPrintable(angleNames[index]
                         + QStringLiteral(" conifer view appears blank")));
            QCOMPARE(coniferAssetPixels(
                    renderedAngles[index], QRect(0, 0, 1280, 24)), 0);
            constexpr int CellWidth = 320;
            for (int cell = 0; cell < 4; ++cell) {
                const QRect content(
                        cell * CellWidth + 12, 24, CellWidth - 24, 620);
                QVERIFY2(coniferAssetPixels(
                        renderedAngles[index], content) > 1000,
                        qPrintable(angleNames[index]
                            + QStringLiteral(" variant %1 is missing")
                                  .arg(cell)));
                QCOMPARE(coniferAssetPixels(
                        renderedAngles[index],
                        QRect(cell * CellWidth, 0, 12, 650)), 0);
                QCOMPARE(coniferAssetPixels(
                        renderedAngles[index],
                        QRect((cell + 1) * CellWidth - 12,
                              0, 12, 650)), 0);
            }
        }
        QVERIFY(changedPixels(renderedAngles[0], renderedAngles[1]) > 1000);
        QVERIFY(changedPixels(renderedAngles[1], renderedAngles[2]) > 1000);
        const QString screenshot = qEnvironmentVariable(
                "GC_WORKOUT_GAME_CONIFER_ASSET_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QVERIFY2(renderedAngles[0].save(screenshot),
                     qPrintable(screenshot));
        }

        QFile pineTrunk(QStringLiteral(
                ":/qml/assets/meshes/geo_ScotsPineTrunk_LOD0_mesh.mesh"));
        QFile pineCrown(QStringLiteral(
                ":/qml/assets/meshes/geo_ScotsPineCrown_LOD0_mesh.mesh"));
        QFile birchTrunk(QStringLiteral(
                ":/qml/assets/meshes/geo_BirchTrunk_LOD0_mesh.mesh"));
        QFile birchCrown(QStringLiteral(
                ":/qml/assets/meshes/geo_BirchCrown_LOD0_mesh.mesh"));
        QVERIFY(pineTrunk.open(QIODevice::ReadOnly));
        QVERIFY(pineCrown.open(QIODevice::ReadOnly));
        QVERIFY(birchTrunk.open(QIODevice::ReadOnly));
        QVERIFY(birchCrown.open(QIODevice::ReadOnly));
        QCOMPARE(pineTrunk.size(), qint64(2632));
        QCOMPARE(pineCrown.size(), qint64(6948));
        QCOMPARE(birchTrunk.size(), qint64(2624));
        QCOMPARE(birchCrown.size(), qint64(6944));
    }

    void rendersPackagedForestDressingCatalog()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.setSource(QUrl(QStringLiteral(
                "qrc:/qml/assets/ForestDressingAssetHarness.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        window.resize(1280, 720);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        QTest::qWait(500);

        const QImage rendered = window.grabWindow();
        QVERIFY(!rendered.isNull());
        QCOMPARE(rendered.size(), QSize(1280, 720));
        QVERIFY2(sampledColorCount(rendered) > 18,
                 "packaged forest-dressing catalog appears blank");
        const QString screenshot = qEnvironmentVariable(
                "GC_WORKOUT_GAME_FOREST_DRESSING_ASSET_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QVERIFY2(rendered.save(screenshot), qPrintable(screenshot));
        }

        QCOMPARE(window.rootObject()->property(
                    "loadedFloorProps").toInt(), 8);
        QCOMPARE(window.rootObject()->property(
                    "loadedVergeClusters").toInt(), 3);
    }

    void rendersForestDressingWithoutObscuringTheChaseScene()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const WorkoutGameCourse course = longFlowingMtbCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        WorkoutGameVisualSnapshot frame = frameAt(road, 42.0);
        frame.simulation.workoutTimeMs = 0;
        frame.presentationTimeMs = 0;
        viewModel.setFrame(frame, 225.0, 225.0, 88, 150, 7);
        constexpr int SettleFrames = 313;
        for (int index = 1; index <= SettleFrames; ++index) {
            const double progress = double(index) / double(SettleFrames);
            frame = frameAt(road, 42.0 + 18.0 * progress);
            frame.simulation.workoutTimeMs = index * 16;
            frame.presentationTimeMs = frame.simulation.workoutTimeMs;
            viewModel.setFrame(frame, 225.0, 225.0, 88, 150, 7);
        }
        QCOMPARE(viewModel.cameraPresentation(), QStringLiteral("chase"));

        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        window.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        window.resize(1280, 720);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        QTest::qWait(700);

        const QList<QObject *> floorProps =
                window.rootObject()->findChildren<QObject *>(
                    QStringLiteral("workoutGameForestFloorProp"));
        const QList<QObject *> vergeClusters =
                window.rootObject()->findChildren<QObject *>(
                    QStringLiteral("workoutGameForestVergeCluster"));
        QCOMPARE(floorProps.size(), 4);
        QCOMPARE(vergeClusters.size(), 3);

        const QImage dressed = window.grabWindow();
        QVERIFY(!dressed.isNull());
        const QString screenshot = qEnvironmentVariable(
                "GC_WORKOUT_GAME_FOREST_DRESSING_SCENE_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QVERIFY2(dressed.save(screenshot), qPrintable(screenshot));
        }
        QVERIFY2(sampledColorCount(dressed) > 45,
                 "forest-dressed chase scene appears blank");
        const QRect lowerScene(
                0, dressed.height() * 2 / 5,
                dressed.width(), dressed.height() * 3 / 5);
        QVERIFY2(riderBluePixels(dressed, lowerScene) > 20,
                 "forest dressing obscures the rider");
        const QVariantList riderPoints = window.rootObject()->property(
                "riderWheelFrustumScreenPoints").toList();
        QVERIFY(riderPoints.size() >= 17);
        QRectF riderBounds;
        for (const QVariant &pointValue : riderPoints) {
            const QVector3D point = pointValue.value<QVector3D>();
            riderBounds |= QRectF(point.x(), point.y(), 1.0, 1.0);
        }
        const int trailTop = std::clamp(
                int(std::floor(riderBounds.bottom() - 8.0)),
                lowerScene.top(), dressed.height() - 1);
        const QRect trailRegion(
                dressed.width() * 3 / 10, trailTop,
                dressed.width() * 4 / 10, dressed.height() - trailTop);
        QVERIFY2(trailDirtPixels(dressed, trailRegion) > 120,
                 "forest dressing obscures the singletrack");

        for (QObject *object : floorProps + vergeClusters) {
            QVERIFY(object->setProperty("visible", false));
        }
        window.update();
        QTest::qWait(200);
        const QImage bare = window.grabWindow();
        QVERIFY(!bare.isNull());
        QVERIFY2(changedPixels(dressed, bare) > 20,
                 "forest dressing has no visible production-scene pixels");

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
        QVERIFY(viewModel.trees().size() >= 12);
        QVERIFY(viewModel.trees().size() <= 18);
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
            const auto roadPiece = std::find_if(
                    road.pieces.begin(), road.pieces.end(),
                    [](const WorkoutGameRoadPiece &piece) {
                        return piece.challenge.enabled
                                || piece.terrain
                                    == WorkoutGameTerrainKind::Berm;
                    });
            QVERIFY(roadPiece != road.pieces.end());
            const WorkoutGameRoadTimelineSection &timeline =
                    road.timeline.front();
            const double distance = std::max(
                    timeline.startDistanceMeters,
                    (entry.terrain == WorkoutGameTerrainKind::Berm
                        ? roadPiece->geometryAnchorDistanceMeters
                        : roadPiece->challenge.obstacleDistanceMeters) - 10.0);
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
            simulation.challenge = roadPiece->challenge.profile;
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
            frame.feature = runtime.update(simulation, 220.0, 220.0);
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

    void criticalFeatureSilhouettesReadAtDecisionDistance_data()
    {
        QTest::addColumn<int>("terrainValue");
        QTest::addColumn<QString>("featureName");
        QTest::addColumn<QString>("modelName");
        QTest::addColumn<int>("minimumWidth");
        QTest::addColumn<int>("minimumHeight");
        QTest::newRow("skinny")
                << int(WorkoutGameTerrainKind::Skinny)
                << QStringLiteral("skinny")
                << QStringLiteral("skinnyGeometryModel") << 40 << 20;
        QTest::newRow("bunny-hop")
                << int(WorkoutGameTerrainKind::BunnyHop)
                << QStringLiteral("bunny-hop")
                << QStringLiteral("GEO_BunnyHopHurdle_LOD0") << 50 << 8;
        QTest::newRow("drop")
                << int(WorkoutGameTerrainKind::Drop)
                << QStringLiteral("drop")
                << QStringLiteral("GEO_DropFace_LOD0") << 70 << 12;
    }

    void criticalFeatureSilhouettesReadAtDecisionDistance()
    {
        QFETCH(int, terrainValue);
        QFETCH(QString, featureName);
        QFETCH(QString, modelName);
        QFETCH(int, minimumWidth);
        QFETCH(int, minimumHeight);
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const WorkoutGameTerrainKind terrain =
                WorkoutGameTerrainKind(terrainValue);
        const WorkoutGameCourse course = catalogCourse(terrain);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());

        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.resize(1280, 720);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        window.setCourse(course, FtpWatts);
        QSignalSpy frameSwaps(&window, &QQuickWindow::frameSwapped);
        auto *rootItem = qobject_cast<QQuickItem *>(window.rootObject());
        auto *viewModel = qobject_cast<WorkoutGame3DViewModel *>(
                window.rootContext()->contextProperty(
                    QStringLiteral("workoutGame3D")).value<QObject *>());
        QVERIFY(rootItem);
        QVERIFY(viewModel);
        QObject *featureModel = rootItem->findChild<QObject *>(modelName);
        QVERIFY2(featureModel, qPrintable(modelName));
        double currentDistance =
                piece->challenge.obstacleDistanceMeters - 10.0;
        qint64 currentTimeMs = settleChaseCamera(
                window, road, currentDistance, 220.0, 220.0, 88, 150, 7);

        const QString outputDirectory = qEnvironmentVariable(
                "GC_WORKOUT_GAME_B1_SCREENSHOT_DIR");
        if (!outputDirectory.isEmpty()) QVERIFY(QDir().mkpath(outputDirectory));
        for (const double approachMeters : {10.0, 8.0}) {
            const double distance =
                    piece->challenge.obstacleDistanceMeters - approachMeters;
            if (distance > currentDistance) {
                constexpr int AdvanceFrames = 60;
                for (int index = 1; index <= AdvanceFrames; ++index) {
                    const double progress = double(index) / AdvanceFrames;
                    WorkoutGameVisualSnapshot advance = frameAt(
                            road,
                            currentDistance
                                + (distance - currentDistance) * progress);
                    advance.simulation.workoutTimeMs =
                            currentTimeMs + index * 16;
                    advance.presentationTimeMs =
                            advance.simulation.workoutTimeMs;
                    window.setFrame(
                            advance, 220.0, 220.0, 88, 150, 7);
                }
                currentTimeMs += AdvanceFrames * 16;
                currentDistance = distance;
            }
            WorkoutGameVisualSnapshot frame = frameAt(road, distance);
            frame.simulation.workoutTimeMs = currentTimeMs;
            frame.presentationTimeMs = frame.simulation.workoutTimeMs;
            window.setFrame(frame, 220.0, 220.0, 88, 150, 7);
            QTest::qWait(350);
            QCOMPARE(viewModel->cameraPresentation(), QStringLiteral("chase"));
            QCOMPARE(viewModel->cameraPresentationBlend(), 0.0);
            const QImage visible = withHudMasked(window.grabWindow(), rootItem);
            QVERIFY(!visible.isNull());

            const int swapsBeforeHide = frameSwaps.count();
            QVERIFY(featureModel->property("visible").toBool());
            QVERIFY(featureModel->setProperty("visible", false));
            QCOMPARE(featureModel->property("visible").toBool(), false);
            window.update();
            QTRY_VERIFY_WITH_TIMEOUT(
                    frameSwaps.count() > swapsBeforeHide, 2000);
            QTest::qWait(120);
            QCOMPARE(featureModel->property("visible").toBool(), false);
            const QImage hidden = withHudMasked(window.grabWindow(), rootItem);
            QVERIFY(!hidden.isNull());
            const int swapsBeforeRestore = frameSwaps.count();
            QVERIFY(featureModel->setProperty("visible", true));
            window.update();
            QTRY_VERIFY_WITH_TIMEOUT(
                    frameSwaps.count() > swapsBeforeRestore, 2000);
            QTest::qWait(80);

            const QRect decisionCorridor(
                    visible.width() / 5,
                    visible.height() / 6,
                    visible.width() * 3 / 5,
                    visible.height() * 7 / 10);
            const QRect silhouette = changedPixelBounds(
                    visible, hidden, decisionCorridor);
            if (!outputDirectory.isEmpty()) {
                const QString stem = QStringLiteral("%1-%2m")
                        .arg(featureName).arg(qRound(approachMeters));
                QVERIFY(visible.save(QDir(outputDirectory).filePath(
                        stem + QStringLiteral("-visible.png"))));
                QVERIFY(hidden.save(QDir(outputDirectory).filePath(
                        stem + QStringLiteral("-hidden.png"))));
            }
            QVERIFY2(silhouette.width() >= minimumWidth
                         && silhouette.height() >= minimumHeight,
                     qPrintable(QStringLiteral(
                         "%1 silhouette at %2 m is only %3x%4 pixels")
                         .arg(featureName).arg(approachMeters)
                         .arg(silhouette.width()).arg(silhouette.height())));
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
        QSignalSpy frameSwaps(&window, &QQuickWindow::frameSwapped);

        const qint64 settledTimeMs = settleChaseCamera(
                window, road, distance, 235.0, 220.0, 92, 152, 7);

        WorkoutGameVisualSnapshot main = frameAt(road, distance);
        main.simulation.workoutTimeMs = settledTimeMs;
        main.presentationTimeMs = main.simulation.workoutTimeMs;
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
        auto *rootItem = qobject_cast<QQuickItem *>(window.rootObject());
        auto *viewModel = qobject_cast<WorkoutGame3DViewModel *>(
                window.rootContext()->contextProperty(
                    QStringLiteral("workoutGame3D")).value<QObject *>());
        QVERIFY(rootItem);
        QVERIFY(viewModel);
        QCOMPARE(viewModel->cameraPresentation(), QStringLiteral("chase"));
        QCOMPARE(viewModel->cameraPresentationBlend(), 0.0);
        QObject *camera = rootItem->findChild<QObject *>(
                QStringLiteral("workoutGameCamera"));
        QVERIFY(camera);
        QObject *cameraTarget = camera->property("lookAtNode").value<QObject *>();
        QVERIFY(cameraTarget);
        const QVector3D fixedCameraPosition =
                camera->property("position").value<QVector3D>();
        const QVector3D fixedCameraTarget =
                cameraTarget->property("position").value<QVector3D>();
        const double fixedFieldOfView =
                camera->property("fieldOfView").toDouble();
        const QImage completed = withHudMasked(window.grabWindow(), rootItem);
        QVERIFY(!completed.isNull());
        QVERIFY(sampledColorCount(completed) > 35);

        WorkoutGameVisualSnapshot bypassed = frameAt(road, distance);
        bypassed.simulation.workoutTimeMs = main.simulation.workoutTimeMs;
        bypassed.presentationTimeMs = main.presentationTimeMs;
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
        window.setFrame(bypassed, 150.0, 220.0, 72, 145, 4);
        QTest::qWait(350);
        QCOMPARE(viewModel->cameraPresentation(), QStringLiteral("chase"));
        QCOMPARE(viewModel->cameraPresentationBlend(), 0.0);
        QVERIFY(camera->setProperty("position", fixedCameraPosition));
        QVERIFY(cameraTarget->setProperty("position", fixedCameraTarget));
        QVERIFY(camera->setProperty("fieldOfView", fixedFieldOfView));
        window.update();
        QCOMPARE(camera->property("position").value<QVector3D>(),
                 fixedCameraPosition);
        QCOMPARE(cameraTarget->property("position").value<QVector3D>(),
                 fixedCameraTarget);
        QTRY_VERIFY_WITH_TIMEOUT(
                std::abs(camera->property("fieldOfView").toDouble()
                         - fixedFieldOfView) < 0.01,
                1000);
        const QImage bypass = withHudMasked(window.grabWindow(), rootItem);
        QVERIFY(!bypass.isNull());
        QVERIFY(sampledColorCount(bypass) > 35);
        QVERIFY2(changedPixels(completed, bypass) > 120,
                 qPrintable(featureName
                     + QStringLiteral(" completed and bypassed lines look identical")));

        QObject *bypassModel = rootItem->findChild<QObject *>(
                QStringLiteral("bypassGeometryModel"));
        QVERIFY(bypassModel);
        auto *renderedBypass = qobject_cast<WorkoutGame3DGeometry *>(
                bypassModel->property("geometry").value<QObject *>());
        QVERIFY(renderedBypass);
        QVERIFY(renderedBypass->ready());
        const int swapsBeforeHide = frameSwaps.count();
        QVERIFY(bypassModel->property("visible").toBool());
        QVERIFY(bypassModel->setProperty("visible", false));
        QCOMPARE(bypassModel->property("visible").toBool(), false);
        window.update();
        QTRY_VERIFY_WITH_TIMEOUT(frameSwaps.count() > swapsBeforeHide, 2000);
        QTest::qWait(120);
        QCOMPARE(bypassModel->property("visible").toBool(), false);
        const QImage withoutBypass = withHudMasked(
                window.grabWindow(), rootItem);
        QVERIFY(!withoutBypass.isNull());
        const int visibleBypassPixels = changedPixels(bypass, withoutBypass);
        QVERIFY(bypassModel->setProperty("visible", true));
        window.update();

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

    void exportsBankedBermLowCenterAndHighLineMotionFrames()
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
                    return candidate.terrain == WorkoutGameTerrainKind::Berm;
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
        const double start = piece->geometryAnchorDistanceMeters
                + profile.startMeters - 2.0;
        const double end = piece->geometryAnchorDistanceMeters
                + profile.endMeters + 2.0;

        std::array<double, 3> maximumRolls = {{0.0, 0.0, 0.0}};
        std::array<double, 3> maximumLaterals = {{0.0, 0.0, 0.0}};
        const std::array<double, 3> lineBiases = {{-1.0, 0.0, 1.0}};
        for (std::size_t lineIndex = 0;
             lineIndex < lineBiases.size(); ++lineIndex) {
            const double lineBias = lineBiases[lineIndex];
            window.setCourse(course, FtpWatts);
            const QString lineName = lineBias < 0.0
                    ? QStringLiteral("low-line")
                    : lineBias > 0.0
                    ? QStringLiteral("high-line")
                    : QStringLiteral("center-line");
            const QString directory = QDir(outputRoot).filePath(
                    lineName);
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
                frame.simulation.workoutTimeMs = std::int64_t(std::llround(
                        (distance - start) / (20.0 / 3.6) * 1000.0));
                frame.simulation.route = WorkoutGameRoute::MainLine;
                frame.simulation.featureOutcome =
                        WorkoutGameFeatureOutcome::None;
                frame.simulation.speedKph = 20.0;
                const double watts = 220.0 * (1.0 + 0.40 * lineBias);
                frame.feature = runtime.update(
                        frame.simulation, watts, 220.0);
                frame.world.terrain = WorkoutGameTerrainKind::Berm;
                frame.world.rider.airborne = false;
                window.setFrame(frame, watts, 220.0, 88, 150, 7);
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
            maximumLaterals[lineIndex] = maximumLateral;
            maximumRolls[lineIndex] = maximumRoll;
        }
        QVERIFY2(maximumLaterals[0] > 0.50,
                 qPrintable(QStringLiteral(
                     "berm low-line maximum lateral was %1 m")
                     .arg(maximumLaterals[0], 0, 'f', 6)));
        QCOMPARE(maximumLaterals[1], 0.0);
        QVERIFY2(maximumLaterals[2] > 0.50,
                 qPrintable(QStringLiteral(
                     "berm high-line maximum lateral was %1 m")
                     .arg(maximumLaterals[2], 0, 'f', 6)));
        QVERIFY(maximumRolls[0] < maximumRolls[1]);
        QVERIFY(maximumRolls[1] < maximumRolls[2]);
        QVERIFY(maximumRolls[2] <= 38.0);
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
                QVERIFY2(largestAirMeters <= maximumExpectedAirMeters,
                         qPrintable(QStringLiteral(
                             "%1 maximum air was %2 m (limit %3 m)")
                             .arg(QString::fromLatin1(videoEnvironment))
                             .arg(largestAirMeters, 0, 'f', 6)
                             .arg(maximumExpectedAirMeters, 0, 'f', 6)));
            } else {
                QCOMPARE(largestAirMeters, 0.0);
            }
        }
    }

    void exportsApprovedCameraCompositionStill()
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
        const WorkoutGameCourse course = sampleCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const WorkoutGameVisualSnapshot frame = frameAt(
                road, tabletopApproachDistance(road));
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
        const QString output = QDir(outputDirectory).filePath(
                QStringLiteral("camera-medium-centre.png"));
        QVERIFY2(rendered.save(output), qPrintable(output));

    }

    void exportsBikeSidePresentationStates()
    {
        const QByteArray requestedOutput =
                qgetenv("GC_WORKOUT_GAME_3D_RIDER_REVIEW_DIR");
        if (requestedOutput.isEmpty()) {
            QSKIP("Set GC_WORKOUT_GAME_3D_RIDER_REVIEW_DIR to export frames");
        }
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const QString outputDirectory =
                QString::fromLocal8Bit(requestedOutput);
        QVERIFY(QDir().mkpath(outputDirectory));
        const WorkoutGameCourse course = cameraMotionCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        constexpr double distanceMeters = 18.0;

        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.setCourse(course, FtpWatts);
        window.resize(1280, 720);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        QObject *camera = window.rootObject()->findChild<QObject *>(
                QStringLiteral("workoutGameCamera"));
        QVERIFY(camera);
        const auto capture = [&](const QString &name,
                                 std::int64_t workoutTimeMs,
                                 double watts,
                                 int cadenceRpm,
                                 double pedalCycles,
                                 QImage *captured) {
            WorkoutGameVisualSnapshot frame = frameAt(road, distanceMeters);
            frame.simulation.workoutTimeMs = workoutTimeMs;
            frame.presentationTimeMs = workoutTimeMs;
            frame.riderPedalCycles = pedalCycles;
            window.setFrame(frame, watts, 180.0, cadenceRpm, 120, 5);
            QTest::qWait(250);
            const QImage image = window.grabWindow();
            QVERIFY(!image.isNull());
            QCOMPARE(image.size(), QSize(1280, 720));
            QVERIFY(sampledColorCount(image) > 35);
            const QString path = QDir(outputDirectory).filePath(
                    name + QStringLiteral(".png"));
            QVERIFY2(image.save(path), qPrintable(path));
            if (captured) *captured = image;
        };

        QImage opening;
        QImage chase;
        QImage idle;
        std::array<QImage, 4> pedalPhases;
        capture(QStringLiteral("rider-opening-side"),
                0, 0.0, 0, 0.0, &opening);
        QTRY_VERIFY_WITH_TIMEOUT(
                std::abs(camera->property("fieldOfView").toDouble() - 41.0)
                    < 0.2,
                1500);
        for (int phase = 0; phase < 4; ++phase) {
            capture(QStringLiteral("rider-pedal-%1")
                        .arg(phase * 90, 3, 10, QLatin1Char('0')),
                    phase == 0 ? 0 : phase * 900,
                    180.0, 85, double(phase) * 0.25,
                    &pedalPhases[std::size_t(phase)]);
        }
        for (std::size_t phase = 1; phase < pedalPhases.size(); ++phase) {
            QVERIFY(changedPixels(
                        pedalPhases[phase - 1], pedalPhases[phase]) > 40);
        }
        capture(QStringLiteral("rider-chase"),
                5000, 180.0, 85, 1.0, &chase);
        QTRY_VERIFY_WITH_TIMEOUT(
                std::abs(camera->property("fieldOfView").toDouble() - 47.0)
                    < 0.2,
                1500);
        capture(QStringLiteral("rider-idle-delay"),
                5001, 0.0, 0, 1.0, nullptr);
        capture(QStringLiteral("rider-idle-side"),
                10901, 0.0, 0, 1.0, &idle);
        QTRY_VERIFY_WITH_TIMEOUT(
                std::abs(camera->property("fieldOfView").toDouble() - 41.0)
                    < 0.2,
                1500);
        QVERIFY(changedPixels(opening, chase) > 500);
        QVERIFY(changedPixels(idle, chase) > 500);
    }

    void exportsApprovedCameraMotionFrames()
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
        const WorkoutGameCourse course = cameraMotionCourse();
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        QCOMPARE(road.totalLengthMeters, 102.0);
        const double startDistance = 2.0;
        const double endDistance = road.totalLengthMeters - 2.0;
        const double speedKph = (endDistance - startDistance)
                / (double(frameCount - 1) / double(frameRate)) * 3.6;
        const QString compositionDirectory = QDir(outputDirectory).filePath(
                QStringLiteral("medium-centre"));
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

        for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
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
            if (!prior.isNull() && changedPixels(prior, rendered) > 40) {
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
                     "medium-centre camera moved in only %1 of %2 transitions")
                     .arg(visiblyChangedFrames)
                     .arg(frameCount - 1)));
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

    void nonPhysicalFeatureHeightUsesOneRoadDatum_data()
    {
        QTest::addColumn<int>("terrain");
        QTest::newRow("bunny-hop")
                << int(WorkoutGameTerrainKind::BunnyHop);
        QTest::newRow("log-over")
                << int(WorkoutGameTerrainKind::LogOver);
    }

    void nonPhysicalFeatureHeightUsesOneRoadDatum()
    {
        QFETCH(int, terrain);
        const WorkoutGameTerrainKind kind = WorkoutGameTerrainKind(terrain);
        const WorkoutGameCourse course = catalogCourse(kind);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const auto piece = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [kind](const WorkoutGameRoadPiece &candidate) {
                    return candidate.terrain == kind
                            && candidate.challenge.enabled;
                });
        QVERIFY(piece != road.pieces.end());
        double distance = piece->challenge.bypassStartDistanceMeters;
        WorkoutGameRoadSample sample;
        for (int index = 0; index <= 100; ++index) {
            const double candidateDistance =
                    piece->challenge.bypassStartDistanceMeters
                    + (piece->challenge.bypassEndDistanceMeters
                       - piece->challenge.bypassStartDistanceMeters)
                        * double(index) / 100.0;
            const WorkoutGameRoadSample candidate =
                    WorkoutGameRoadCourseBuilder::sample(
                        road, candidateDistance);
            if (!sample.ready
                    || candidate.nonPhysicalFeatureOffsetMeters
                        > sample.nonPhysicalFeatureOffsetMeters) {
                distance = candidateDistance;
                sample = candidate;
            }
        }
        QVERIFY(sample.ready);
        if (kind == WorkoutGameTerrainKind::LogOver) {
            QVERIFY(sample.nonPhysicalFeatureOffsetMeters > 0.01);
        } else {
            QCOMPARE(sample.nonPhysicalFeatureOffsetMeters, 0.0);
        }

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        WorkoutGameVisualSnapshot frame = frameAt(road, distance);
        frame.feature.ready = true;
        frame.feature.terrain = kind;
        frame.feature.route = WorkoutGameRoute::MainLine;
        frame.feature.motion = WorkoutGameFeatureMotion::Jump;
        frame.feature.outcome = WorkoutGameFeatureOutcome::None;
        frame.world.rider.airborne = false;
        frame.world.rider.clearanceMeters = 0.82;
        viewModel.setFrame(frame, 220.0, 220.0, 86, 148, 6);
        QVERIFY(std::abs(viewModel.groundY()
                    - sample.visualGroundElevationMeters()) < 1e-9);
        QVERIFY(std::abs(viewModel.riderY()
                    - sample.visualGroundElevationMeters()) < 1e-9);

        frame.feature.outcome = WorkoutGameFeatureOutcome::Completed;
        frame.world.rider.airborne = true;
        frame.world.rider.clearanceMeters = 1.22;
        viewModel.setFrame(frame, 250.0, 220.0, 92, 152, 8);
        QVERIFY(std::abs(viewModel.groundY()
                    - sample.visualGroundElevationMeters()) < 1e-9);
        QVERIFY(std::abs(viewModel.riderY()
                    - sample.visualGroundElevationMeters() - 0.40) < 1e-9);
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

    void visualPitchIsBoundedWithoutChangingThePhysicsSnapshot()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::SmoothTrail);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);

        WorkoutGameVisualSnapshot frame = frameAt(road, 8.0);
        frame.world.rider.pitchDegrees = 144.0;
        viewModel.setFrame(frame, 180.0, 180.0, 86, 148, 6);
        QCOMPARE(viewModel.riderPitch(), 35.0);
        QCOMPARE(frame.world.rider.pitchDegrees, 144.0);

        frame.world.rider.pitchDegrees = -171.0;
        viewModel.setFrame(frame, 180.0, 180.0, 86, 148, 6);
        QCOMPARE(viewModel.riderPitch(), -35.0);
        QCOMPARE(frame.world.rider.pitchDegrees, -171.0);
    }

    void qmlConvertsPhysicsPitchToQuick3DCoordinates()
    {
        const WorkoutGameCourse course = catalogCourse(
                WorkoutGameTerrainKind::Climb);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);

        WorkoutGameVisualSnapshot frame = frameAt(road, 8.0);
        frame.world.rider.pitchDegrees = 12.5;
        viewModel.setFrame(frame, 220.0, 220.0, 86, 148, 7);

        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.resize(960, 540);
        window.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        window.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);

        QObject *rider = window.rootObject()->findChild<QObject *>(
                QStringLiteral("riderNode"));
        QVERIFY(rider);
        QCOMPARE(viewModel.riderPitch(), 12.5);
        QCOMPARE(rider->property("riderPitch").toDouble(), -12.5);
        QCOMPARE(frame.world.rider.pitchDegrees, 12.5);
    }

    void authoredGapJumpAssetReplacesProceduralQuick3DSurface()
    {
        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 1701u;
        course.durationMs = 30000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::SprintJump;
        section.terrain = WorkoutGameTerrainKind::GapJump;
        section.durationMs = course.durationMs;
        section.lengthMeters = 120.0;
        section.targetWatts = 260.0;
        section.difficulty = 0.5;
        section.challengeCount = 1;
        course.sections = {section};

        WorkoutGame3DViewModel viewModel;
        viewModel.setCourse(course, FtpWatts);
        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        const auto gate = std::find_if(
                road.pieces.begin(), road.pieces.end(),
                [](const WorkoutGameRoadPiece &piece) {
                    return piece.gapJump.enabled;
                });
        QVERIFY(gate != road.pieces.end());
        const double reviewDistance = std::max(
                0.0, gate->gapJump.splitStartDistanceMeters - 5.0);
        viewModel.setFrame(
                frameAt(road, reviewDistance), 260.0, 260.0, 92, 154, 8);
        auto *geometry = qobject_cast<WorkoutGame3DGeometry *>(
                viewModel.gapJumpGeometry());
        QVERIFY(geometry);
        QVERIFY(!geometry->ready());
        QCOMPARE(geometry->triangleCount(), 0);

        QQuickView window;
        window.setResizeMode(QQuickView::SizeRootObjectToView);
        window.resize(960, 540);
        window.rootContext()->setContextProperty(
                QStringLiteral("workoutGame3D"), &viewModel);
        window.setSource(QUrl(QStringLiteral("qrc:/qml/WorkoutGame3D.qml")));
        QCOMPARE(window.status(), QQuickView::Ready);
        QVERIFY(!window.rootObject()->findChild<QObject *>(
                QStringLiteral("gapJumpGeometryModel")));
        const QList<QObject *> assets = window.rootObject()->findChildren<QObject *>(
                QStringLiteral("gapJumpAssetInstance"));
        QCOMPARE(assets.size(), 1);
        QVERIFY(assets.front()->property("visible").toBool());
        const WorkoutGameRoadSample socket =
                WorkoutGameRoadCourseBuilder::sample(
                    road, gate->gapJump.splitStartDistanceMeters);
        QVERIFY(socket.ready);
        QCOMPARE(assets.front()->property("position").value<QVector3D>(),
                 QVector3D(float(socket.center.xMeters),
                           float(socket.visualGroundElevationMeters()),
                           float(socket.center.zMeters)));
    }

    void exportsGapJumpAcceptanceMatrixAndRunsSimulatedEndurance()
    {
        if (!hasInteractiveGraphicsPlatform()) {
            QSKIP("Quick 3D rendering requires an interactive GPU platform");
        }
        const QString outputRoot = qEnvironmentVariable(
                "GC_WORKOUT_GAME_GAP_JUMP_ACCEPTANCE_DIR");
        if (outputRoot.isEmpty()) {
            QSKIP("Set GC_WORKOUT_GAME_GAP_JUMP_ACCEPTANCE_DIR to run FTR-12");
        }
        QVERIFY(QDir().mkpath(outputRoot));

        WorkoutGameCourse course;
        course.status = WorkoutGameCourseStatus::Ready;
        course.seed = 1701u;
        course.durationMs = 30000;
        WorkoutGameSection section;
        section.feature = WorkoutGameFeature::SprintJump;
        section.terrain = WorkoutGameTerrainKind::GapJump;
        section.durationMs = course.durationMs;
        section.lengthMeters = 120.0;
        section.targetWatts = 260.0;
        section.difficulty = 0.5;
        section.challengeCount = 1;
        course.sections = {section};

        const WorkoutGameRoadCourse road =
                WorkoutGameRoadCourseBuilder::build(course, FtpWatts);
        QVERIFY(road.ready);
        const auto piece = std::find_if(
                road.pieces.cbegin(), road.pieces.cend(),
                [](const WorkoutGameRoadPiece &candidate) {
                    return candidate.gapJump.enabled;
                });
        QVERIFY(piece != road.pieces.cend());
        const WorkoutGameRoadGapJumpGate &gate = piece->gapJump;
        const double fallbackLateralMeters =
                piece->challenge.bypassLateralMeters;

        struct Scenario
        {
            WorkoutGameGapJumpLine line;
            const char *name;
            bool fallback;
        };
        const std::array<Scenario, 4> scenarios = {{
            {WorkoutGameGapJumpLine::Short, "short", false},
            {WorkoutGameGapJumpLine::Medium, "medium", false},
            {WorkoutGameGapJumpLine::Long, "long", false},
            {WorkoutGameGapJumpLine::None, "fallback", true}
        }};
        const auto roadLine = [&gate](WorkoutGameGapJumpLine id) {
            return std::find_if(
                    gate.lines.cbegin(), gate.lines.cend(),
                    [id](const WorkoutGameRoadGapJumpLine &candidate) {
                        return candidate.id == id;
                    });
        };
        const auto smoothStep = [](double progress) {
            const double bounded = std::clamp(progress, 0.0, 1.0);
            return bounded * bounded * (3.0 - 2.0 * bounded);
        };
        const auto lateralAt = [&gate, &smoothStep](
                double distanceMeters, double selectedLateralMeters) {
            const double splitProgress = (distanceMeters
                    - gate.splitStartDistanceMeters)
                    / std::max(0.01,
                        gate.lines.front().takeoffDistanceMeters
                            - gate.splitStartDistanceMeters);
            const double mergeStart = gate.lines.back().landingDistanceMeters;
            const double mergeProgress = (distanceMeters - mergeStart)
                    / std::max(0.01,
                        gate.mergeEndDistanceMeters - mergeStart);
            return selectedLateralMeters
                    * smoothStep(splitProgress)
                    * (1.0 - smoothStep(mergeProgress));
        };
        const auto acceptanceFrame = [
                &road, &gate, &roadLine, &lateralAt,
                fallbackLateralMeters](
                const Scenario &scenario,
                double distanceMeters,
                std::int64_t timeMs) {
            const auto selected = scenario.fallback
                    ? gate.lines.cbegin() : roadLine(scenario.line);
            const double takeoff = selected->takeoffDistanceMeters;
            const double landing = selected->landingDistanceMeters;
            const double selectedLateral = scenario.fallback
                    ? fallbackLateralMeters : selected->lateralMeters;
            const double actionProgress = std::clamp(
                    (distanceMeters - takeoff)
                        / std::max(0.01, landing - takeoff),
                    0.0, 1.0);
            const double airMeters = scenario.fallback
                    || distanceMeters < takeoff || distanceMeters > landing
                ? 0.0
                : std::min(2.4,
                    selected->lipHeightMeters
                        + 0.12 * selected->gapLengthMeters)
                    * std::sin(3.14159265358979323846 * actionProgress);

            WorkoutGameVisualSnapshot frame = frameAt(road, distanceMeters);
            frame.simulation.activeSection = 0;
            frame.simulation.sectionProgress = std::clamp(
                    distanceMeters / road.totalLengthMeters, 0.0, 1.0);
            frame.simulation.workoutTimeMs = timeMs;
            frame.simulation.speedKph = scenario.fallback
                    ? 12.0 : selected->minimumSpeedMetersPerSecond * 3.6;
            frame.presentationTimeMs = timeMs;
            frame.feature.ready = true;
            frame.feature.terrain = WorkoutGameTerrainKind::GapJump;
            frame.feature.motion = WorkoutGameFeatureMotion::Jump;
            frame.feature.outcome = scenario.fallback
                    ? WorkoutGameFeatureOutcome::Bypassed
                    : WorkoutGameFeatureOutcome::Completed;
            frame.feature.route = scenario.fallback
                    ? WorkoutGameRoute::SafeBypass
                    : WorkoutGameRoute::MainLine;
            frame.feature.prepareDistanceMeters = gate.prepareDistanceMeters;
            frame.feature.launchWindowStartDistanceMeters =
                    gate.launchWindowStartDistanceMeters;
            frame.feature.decisionDistanceMeters = gate.lockDistanceMeters;
            frame.feature.obstacleDistanceMeters = takeoff;
            frame.feature.physicalTakeoffDistanceMeters = takeoff;
            frame.feature.actionStartDistanceMeters = takeoff;
            frame.feature.actionEndDistanceMeters = landing;
            frame.feature.visualDistanceMeters = distanceMeters;
            frame.feature.distanceToObstacleMeters = takeoff - distanceMeters;
            frame.feature.lateralOffsetMeters = lateralAt(
                    distanceMeters, selectedLateral);
            frame.feature.verticalOffsetMeters = airMeters;
            frame.feature.flightDurationSeconds = scenario.fallback
                    ? 0.0 : selected->nominalFlightSeconds;
            frame.feature.pitchDegrees = scenario.fallback
                    ? 0.0 : 7.0 * std::cos(
                        3.14159265358979323846 * actionProgress);
            frame.feature.gapLineLocked = true;
            frame.feature.lockedGapLine = scenario.line;
            frame.feature.steeringGapLine = scenario.line;
            frame.feature.predictedApproachSpeedMetersPerSecond =
                    scenario.fallback
                        ? 3.0 : selected->minimumSpeedMetersPerSecond;
            frame.feature.actionId = 0x170100u
                    + std::uint64_t(scenario.line);
            if (distanceMeters < gate.lockDistanceMeters) {
                frame.feature.phase = WorkoutGameFeaturePhase::Measure;
            } else if (distanceMeters < takeoff) {
                frame.feature.phase = WorkoutGameFeaturePhase::Committed;
            } else if (distanceMeters < landing) {
                frame.feature.phase = WorkoutGameFeaturePhase::Action;
                frame.feature.triggerJump = !scenario.fallback;
            } else {
                frame.feature.phase = WorkoutGameFeaturePhase::Recovery;
            }
            frame.world.terrain = WorkoutGameTerrainKind::GapJump;
            frame.world.rider.pitchDegrees = frame.feature.pitchDegrees;
            frame.world.rider.airborne = airMeters > 0.01;
            frame.world.rider.clearanceMeters = 0.82 + airMeters;
            frame.world.rider.rearWheelGrounded = airMeters <= 0.01;
            frame.world.rider.frontWheelGrounded = airMeters <= 0.01;
            frame.world.landingImpact = !scenario.fallback
                    && distanceMeters >= landing
                    && distanceMeters <= landing + 0.30 ? 0.85 : 0.0;
            return frame;
        };

        WorkoutGame3DWindow window(true);
        QVERIFY(window.rendererAvailable());
        window.resize(960, 540);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        auto *viewModel = qobject_cast<WorkoutGame3DViewModel *>(
                window.rootContext()->contextProperty(
                    QStringLiteral("workoutGame3D")).value<QObject *>());
        QVERIFY(viewModel);

        constexpr int MotionFrameCount = 72;
        constexpr int FrameRate = 30;
        const std::array<const char *, 4> stillNames = {{
            "approach", "takeoff", "apex", "landing"
        }};
        std::array<QImage, 4> lineApexImages;
        for (std::size_t scenarioIndex = 0;
                scenarioIndex < scenarios.size(); ++scenarioIndex) {
            const Scenario &scenario = scenarios[scenarioIndex];
            const auto selected = scenario.fallback
                    ? gate.lines.cbegin() : roadLine(scenario.line);
            QVERIFY(selected != gate.lines.cend());
            const double takeoff = selected->takeoffDistanceMeters;
            const double landing = selected->landingDistanceMeters;
            const std::array<double, 4> stillDistances = {{
                gate.launchWindowStartDistanceMeters + 1.0,
                takeoff - 0.05,
                takeoff + (landing - takeoff) * 0.5,
                landing + 0.15
            }};
            const QString scenarioDirectory = QDir(outputRoot).filePath(
                    QString::fromLatin1(scenario.name));
            const QString motionDirectory = QDir(scenarioDirectory).filePath(
                    QStringLiteral("motion"));
            QVERIFY(QDir().mkpath(motionDirectory));
            QDir motionFrames(motionDirectory);
            for (const QString &stale : motionFrames.entryList(
                    {QStringLiteral("frame-*.png")}, QDir::Files)) {
                QVERIFY(motionFrames.remove(stale));
            }

            window.setCourse(course, FtpWatts);
            QImage priorStill;
            for (std::size_t stillIndex = 0;
                    stillIndex < stillDistances.size(); ++stillIndex) {
                const WorkoutGameVisualSnapshot frame = acceptanceFrame(
                        scenario, stillDistances[stillIndex],
                        1000 + std::int64_t(stillIndex) * 750);
                window.setFrame(
                        frame,
                        scenario.fallback ? 150.0 : 270.0,
                        260.0,
                        scenario.fallback ? 70 : 94,
                        152,
                        scenario.fallback ? 3 : 9);
                QTest::qWait(120);
                const QImage image = window.grabWindow();
                QVERIFY(!image.isNull());
                QCOMPARE(image.size(), QSize(960, 540));
                QVERIFY2(sampledColorCount(image) > 35,
                         "gap-jump still is blank or nearly monochrome");
                const QString path = QDir(scenarioDirectory).filePath(
                        QString::fromLatin1(stillNames[stillIndex])
                            + QStringLiteral(".png"));
                QVERIFY2(image.save(path), qPrintable(path));
                QVERIFY(QFile::exists(path));
                if (!priorStill.isNull()) {
                    QVERIFY2(changedPixels(priorStill, image) > 40,
                             qPrintable(QStringLiteral(
                                 "%1 still stages are visually identical")
                                 .arg(QString::fromLatin1(scenario.name))));
                }
                priorStill = image;
                if (stillIndex == 2) {
                    lineApexImages[scenarioIndex] = image;
                }
            }
            if (scenario.fallback) {
                QCOMPARE(viewModel->riderPoseState(), QStringLiteral("bypass"));
            } else {
                QCOMPARE(viewModel->riderPoseState(), QStringLiteral("land"));
            }

            const double motionStart = gate.splitStartDistanceMeters - 1.0;
            const double motionEnd = std::min(
                    road.totalLengthMeters,
                    gate.mergeEndDistanceMeters + 1.0);
            int visiblyChangedFrames = 0;
            int airborneFrames = 0;
            double maximumAirMeters = 0.0;
            QImage previous;
            for (int frameIndex = 0;
                    frameIndex < MotionFrameCount; ++frameIndex) {
                const double progress = double(frameIndex)
                        / double(MotionFrameCount - 1);
                const double distance = motionStart
                        + (motionEnd - motionStart) * progress;
                const WorkoutGameVisualSnapshot frame = acceptanceFrame(
                        scenario, distance,
                        5000 + frameIndex * (1000 / FrameRate));
                if (frame.world.rider.airborne) ++airborneFrames;
                maximumAirMeters = std::max(
                        maximumAirMeters, frame.world.rider.airHeightMeters());
                window.setFrame(
                        frame,
                        scenario.fallback ? 150.0 : 270.0,
                        260.0,
                        scenario.fallback ? 70 : 94,
                        152,
                        scenario.fallback ? 3 : 9);
                QTest::qWait(4);
                const QImage image = window.grabWindow();
                QVERIFY(!image.isNull());
                QVERIFY(sampledColorCount(image) > 30);
                if (!previous.isNull()
                        && changedPixels(previous, image) > 20) {
                    ++visiblyChangedFrames;
                }
                previous = image;
                const QString path = motionFrames.filePath(
                        QStringLiteral("frame-%1.png")
                            .arg(frameIndex, 4, 10, QLatin1Char('0')));
                QImageWriter writer(path, "png");
                writer.setCompression(1);
                QVERIFY2(writer.write(image), qPrintable(writer.errorString()));
            }
            QVERIFY2(visiblyChangedFrames > MotionFrameCount * 4 / 5,
                     qPrintable(QStringLiteral(
                         "%1 motion changed only %2 of %3 frames")
                         .arg(QString::fromLatin1(scenario.name))
                         .arg(visiblyChangedFrames)
                         .arg(MotionFrameCount)));
            if (scenario.fallback) {
                QCOMPARE(airborneFrames, 0);
                QCOMPARE(maximumAirMeters, 0.0);
            } else {
                QVERIFY(airborneFrames >= 2);
                QVERIFY(maximumAirMeters > 0.45);
                QVERIFY(maximumAirMeters <= 2.4);
            }
        }
        for (std::size_t index = 1; index < lineApexImages.size(); ++index) {
            QVERIFY2(changedPixels(
                        lineApexImages[index - 1], lineApexImages[index]) > 80,
                     "adjacent gap-jump routes are not visually distinct");
        }

        const QString enduranceDirectory = QDir(outputRoot).filePath(
                QStringLiteral("endurance"));
        QVERIFY(QDir().mkpath(enduranceDirectory));
        QDir enduranceOutput(enduranceDirectory);
        for (const QString &stale : enduranceOutput.entryList(
                {QStringLiteral("*.png")}, QDir::Files)) {
            QVERIFY(enduranceOutput.remove(stale));
        }
        QCOMPARE(enduranceOutput.entryList(
                    {QStringLiteral("*.png")}, QDir::Files).size(), 0);
        window.setCourse(course, FtpWatts);
        viewModel->resetFrameWorkCounters();
        constexpr std::int64_t EnduranceDurationMs = 5 * 60 * 1000;
        constexpr std::int64_t SimulationStepMs = 20;
        constexpr std::int64_t LapDurationMs = 6000;
        std::vector<FrameUpdateTiming> updateTimings;
        updateTimings.reserve(
                std::size_t(EnduranceDurationMs / SimulationStepMs));
        int eventPumps = 0;
        for (std::int64_t timeMs = 0;
                timeMs < EnduranceDurationMs; timeMs += SimulationStepMs) {
            const std::size_t lap = std::size_t(timeMs / LapDurationMs);
            const Scenario &scenario = scenarios[lap % scenarios.size()];
            const double progress = double(timeMs % LapDurationMs)
                    / double(LapDurationMs);
            const double distance = gate.splitStartDistanceMeters - 1.0
                    + (gate.mergeEndDistanceMeters
                        - gate.splitStartDistanceMeters + 2.0) * progress;
            const WorkoutGameVisualSnapshot frame = acceptanceFrame(
                    scenario, distance, 10000 + timeMs);
            const ThreadExecutionSnapshot executionBefore =
                    threadExecutionSnapshot();
            QVERIFY2(executionBefore.cpuNanoseconds >= 0,
                     "per-thread CPU diagnostics require Linux thread clocks");
            QElapsedTimer updateTimer;
            updateTimer.start();
            window.setFrame(
                    frame,
                    scenario.fallback ? 150.0 : 270.0,
                    260.0,
                    scenario.fallback ? 70 : 94,
                    152,
                    scenario.fallback ? 3 : 9);
            const qint64 wallNanoseconds = updateTimer.nsecsElapsed();
            const ThreadExecutionSnapshot executionAfter =
                    threadExecutionSnapshot();
            QVERIFY(executionAfter.cpuNanoseconds
                    >= executionBefore.cpuNanoseconds);
            if (timeMs >= 5000) {
                updateTimings.push_back({
                    double(wallNanoseconds) / 1000000.0,
                    double(executionAfter.cpuNanoseconds
                            - executionBefore.cpuNanoseconds) / 1000000.0,
                    executionAfter.voluntaryContextSwitches
                            - executionBefore.voluntaryContextSwitches,
                    executionAfter.involuntaryContextSwitches
                            - executionBefore.involuntaryContextSwitches
                });
            }
            if ((timeMs / SimulationStepMs) % 50 == 0) {
                window.update();
                QGuiApplication::processEvents(QEventLoop::AllEvents, 1);
                ++eventPumps;
            }
        }
        QVERIFY(!updateTimings.empty());
        std::vector<double> wallMilliseconds;
        std::vector<double> threadCpuMilliseconds;
        wallMilliseconds.reserve(updateTimings.size());
        threadCpuMilliseconds.reserve(updateTimings.size());
        double maximumUnpreemptedWallMs = 0.0;
        int hostPreemptionSamples = 0;
        long voluntaryContextSwitches = 0;
        long involuntaryContextSwitches = 0;
        for (const FrameUpdateTiming &timing : updateTimings) {
            wallMilliseconds.push_back(timing.wallMilliseconds);
            threadCpuMilliseconds.push_back(timing.threadCpuMilliseconds);
            voluntaryContextSwitches += timing.voluntaryContextSwitches;
            involuntaryContextSwitches += timing.involuntaryContextSwitches;
            const bool hostPreempted = timing.involuntaryContextSwitches > 0
                    && timing.wallMilliseconds
                            - timing.threadCpuMilliseconds > 2.0;
            if (hostPreempted) {
                ++hostPreemptionSamples;
            } else {
                maximumUnpreemptedWallMs = std::max(
                        maximumUnpreemptedWallMs,
                        timing.wallMilliseconds);
            }
        }
        const double p95WallMs = percentile(wallMilliseconds, 0.95);
        const double p99WallMs = percentile(wallMilliseconds, 0.99);
        const double maximumWallMs = *std::max_element(
                wallMilliseconds.cbegin(), wallMilliseconds.cend());
        const double p95ThreadCpuMs = percentile(
                threadCpuMilliseconds, 0.95);
        const double maximumThreadCpuMs = *std::max_element(
                threadCpuMilliseconds.cbegin(),
                threadCpuMilliseconds.cend());
        qInfo().noquote() << QStringLiteral(
                "setFrame endurance: wall p95=%1 ms p99=%2 ms max=%3 ms; "
                "thread CPU p95=%4 ms max=%5 ms; unpreempted wall max=%6 "
                "ms; host preemption samples=%7; context switches=%8/%9")
            .arg(p95WallMs, 0, 'f', 3)
            .arg(p99WallMs, 0, 'f', 3)
            .arg(maximumWallMs, 0, 'f', 3)
            .arg(p95ThreadCpuMs, 0, 'f', 3)
            .arg(maximumThreadCpuMs, 0, 'f', 3)
            .arg(maximumUnpreemptedWallMs, 0, 'f', 3)
            .arg(hostPreemptionSamples)
            .arg(voluntaryContextSwitches)
            .arg(involuntaryContextSwitches);
        QVERIFY2(p95WallMs <= 12.0,
                 qPrintable(QStringLiteral(
                     "gap-jump endurance wall p95 was %1 ms")
                     .arg(p95WallMs, 0, 'f', 3)));
        QVERIFY2(p99WallMs <= 20.0,
                 qPrintable(QStringLiteral(
                     "gap-jump endurance wall p99 was %1 ms")
                     .arg(p99WallMs, 0, 'f', 3)));
        QVERIFY2(p95ThreadCpuMs <= 12.0,
                 qPrintable(QStringLiteral(
                     "gap-jump endurance thread CPU p95 was %1 ms")
                     .arg(p95ThreadCpuMs, 0, 'f', 3)));
        QVERIFY2(maximumThreadCpuMs <= 50.0,
                 qPrintable(QStringLiteral(
                     "gap-jump endurance synchronous thread CPU maximum "
                     "was %1 ms")
                     .arg(maximumThreadCpuMs, 0, 'f', 3)));
        QVERIFY2(maximumUnpreemptedWallMs <= 50.0,
                 qPrintable(QStringLiteral(
                     "gap-jump endurance unpreempted wall maximum was %1 ms")
                     .arg(maximumUnpreemptedWallMs, 0, 'f', 3)));
        QVERIFY(viewModel->geometryQueueDepth() <= 1);
        QVERIFY(viewModel->visibleTriangles() > 0);
        QVERIFY(std::isfinite(viewModel->riderX()));
        QVERIFY(std::isfinite(viewModel->riderY()));
        QVERIFY(std::isfinite(viewModel->riderZ()));
        QCOMPARE(enduranceOutput.entryList(
                    {QStringLiteral("*.png")}, QDir::Files).size(), 0);
        const WorkoutGame3DFrameWorkCounters workCounters =
                viewModel->frameWorkCounters();
        qInfo().noquote() << QStringLiteral(
                "setFrame work: frames=%1 scene=%2 telemetry=%3 course=%4 "
                "trees=%5 forest=%6 floor=%7; regenerations=%8/%9/%10; "
                "floor requests/completed/installs=%11/%12/%13; "
                "tree entries visited=%14")
            .arg(workCounters.frameCalls)
            .arg(workCounters.sceneSignals)
            .arg(workCounters.telemetrySignals)
            .arg(workCounters.courseSignals)
            .arg(workCounters.treeSignals)
            .arg(workCounters.forestSignals)
            .arg(workCounters.floorSignals)
            .arg(workCounters.featureModelRegenerations)
            .arg(workCounters.treeModelRegenerations)
            .arg(workCounters.forestModelRegenerations)
            .arg(workCounters.floorBuildRequests)
            .arg(workCounters.floorChunkBuildsCompleted)
            .arg(workCounters.floorChunkInstalls)
            .arg(workCounters.treeClearanceEntriesVisited);

        QJsonObject report;
        report.insert(QStringLiteral("simulated_duration_ms"),
                      qint64(EnduranceDurationMs));
        report.insert(QStringLiteral("simulation_step_ms"),
                      qint64(SimulationStepMs));
        report.insert(QStringLiteral("simulated_frames"),
                      qint64(EnduranceDurationMs / SimulationStepMs));
        report.insert(QStringLiteral("warmup_excluded_ms"), qint64(5000));
        report.insert(QStringLiteral("render_event_pumps"), eventPumps);
        report.insert(QStringLiteral("capture_count"), 0);
        report.insert(QStringLiteral("wall_p95_ms"), p95WallMs);
        report.insert(QStringLiteral("wall_p99_ms"), p99WallMs);
        report.insert(QStringLiteral("wall_maximum_ms"), maximumWallMs);
        report.insert(QStringLiteral("thread_cpu_p95_ms"), p95ThreadCpuMs);
        report.insert(QStringLiteral("thread_cpu_maximum_ms"),
                      maximumThreadCpuMs);
        report.insert(QStringLiteral("unpreempted_wall_maximum_ms"),
                      maximumUnpreemptedWallMs);
        report.insert(QStringLiteral("host_preemption_samples"),
                      hostPreemptionSamples);
        report.insert(QStringLiteral("voluntary_context_switches"),
                      qint64(voluntaryContextSwitches));
        report.insert(QStringLiteral("involuntary_context_switches"),
                      qint64(involuntaryContextSwitches));
        report.insert(QStringLiteral("frame_calls"),
                      qint64(workCounters.frameCalls));
        report.insert(QStringLiteral("scene_signals"),
                      qint64(workCounters.sceneSignals));
        report.insert(QStringLiteral("telemetry_signals"),
                      qint64(workCounters.telemetrySignals));
        report.insert(QStringLiteral("course_signals"),
                      qint64(workCounters.courseSignals));
        report.insert(QStringLiteral("tree_signals"),
                      qint64(workCounters.treeSignals));
        report.insert(QStringLiteral("forest_signals"),
                      qint64(workCounters.forestSignals));
        report.insert(QStringLiteral("floor_signals"),
                      qint64(workCounters.floorSignals));
        report.insert(QStringLiteral("feature_model_regenerations"),
                      qint64(workCounters.featureModelRegenerations));
        report.insert(QStringLiteral("tree_model_regenerations"),
                      qint64(workCounters.treeModelRegenerations));
        report.insert(QStringLiteral("forest_model_regenerations"),
                      qint64(workCounters.forestModelRegenerations));
        report.insert(QStringLiteral("floor_build_requests"),
                      qint64(workCounters.floorBuildRequests));
        report.insert(QStringLiteral("floor_chunk_builds_completed"),
                      qint64(workCounters.floorChunkBuildsCompleted));
        report.insert(QStringLiteral("floor_chunk_installs"),
                      qint64(workCounters.floorChunkInstalls));
        report.insert(QStringLiteral("tree_clearance_entries_visited"),
                      qint64(workCounters.treeClearanceEntriesVisited));
        report.insert(QStringLiteral("geometry_queue_depth"),
                      viewModel->geometryQueueDepth());
        report.insert(QStringLiteral("visible_triangles"),
                      viewModel->visibleTriangles());
        const QString reportPath = enduranceOutput.filePath(
                QStringLiteral("report.json"));
        QFile reportFile(reportPath);
        QVERIFY2(reportFile.open(QIODevice::WriteOnly | QIODevice::Truncate),
                 qPrintable(reportFile.errorString()));
        const QByteArray reportBytes =
                QJsonDocument(report).toJson(QJsonDocument::Indented);
        QCOMPARE(reportFile.write(reportBytes), qint64(reportBytes.size()));
        reportFile.close();
        QVERIFY(QFile::exists(reportPath));
    }
};

QTEST_MAIN(TestWorkoutGame3DView)
#include "testWorkoutGame3DView.moc"
