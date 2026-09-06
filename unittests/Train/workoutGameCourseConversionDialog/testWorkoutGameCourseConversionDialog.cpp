/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/WorkoutGameCourseConversionDialog.h"
#include "Train/WorkoutGameCoursePreviewWidget.h"
#include "Train/WorkoutGameCoursePreviewMetrics.h"
#include "Train/WorkoutGameRoadPlan.h"

#include <QFileInfo>
#include <QImage>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRect>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

namespace {

WorkoutGameCourseSourceRequest sampleRequest()
{
    WorkoutGameCourseSourceRequest request;
    double timeMs = 0.0;
    auto append = [&](double durationMs, double watts) {
        if (request.points.empty()) {
            request.points.push_back({timeMs, watts});
        } else if (request.points.back().watts != watts) {
            request.points.push_back({timeMs, watts});
        }
        timeMs += durationMs;
        request.points.push_back({timeMs, watts});
    };
    append(4 * 60000.0, 135.0);
    append(4 * 60000.0, 145.0);
    for (int repetition = 0; repetition < 3; ++repetition) {
        append(4 * 60000.0, 205.0 + repetition * 3.0);
        append(10000.0, 250.0 + repetition * 5.0);
        append(3 * 60000.0, 110.0);
    }
    append(5 * 60000.0, 95.0);
    request.sourceContents = QByteArrayLiteral("ui-test-source");
    request.sourceFileName = QStringLiteral("intervals.erg");
    request.ftpWatts = 190.0;
    return request;
}

double totalAbsoluteTurn(const WorkoutGameCourseSourceResult &result)
{
    double radians = 0.0;
    if (!result.document.course.roadPlan) return radians;
    for (const WorkoutGameRoadPiece &piece
            : result.document.course.roadPlan->pieces) {
        radians += std::abs(piece.turnRadians);
    }
    return radians;
}

template<typename T>
T *requiredChild(QObject &parent, const char *name)
{
    T *child = parent.findChild<T *>(QLatin1String(name));
    Q_ASSERT(child);
    return child;
}

}

class TestWorkoutGameCourseConversionDialog : public QObject
{
    Q_OBJECT

    void verifyResponsiveGeometry(qreal fontScale)
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        WorkoutGameCourseConversionDialog dialog(
                sampleRequest(), directory.filePath("intervals-mtb.crs"));
        QFont font = dialog.font();
        font.setPointSizeF(font.pointSizeF() * fontScale);
        dialog.setFont(font);
        dialog.resize(820, 700);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QCoreApplication::processEvents();

        QScrollArea *scrollArea = dialog.findChild<QScrollArea *>(
                QStringLiteral("courseDetailsScrollArea"));
        QWidget *scrollContents = dialog.findChild<QWidget *>(
                QStringLiteral("courseDetailsScrollContents"));
        QDialogButtonBox *buttonBox = dialog.findChild<QDialogButtonBox *>(
                QStringLiteral("courseDialogButtonBox"));
        QVERIFY2(scrollArea, "The detail region must be vertically scrollable");
        QVERIFY(scrollContents);
        QVERIFY(buttonBox);

        auto dialogGeometry = [&](QWidget *widget) {
            return QRect(widget->mapTo(&dialog, QPoint(0, 0)), widget->size());
        };
        QWidget *heading = requiredChild<QWidget>(dialog, "courseDialogHeading");
        QWidget *guarantee = requiredChild<QWidget>(
                dialog, "prescriptionGuaranteeLabel");
        QWidget *workoutFirst = requiredChild<QWidget>(
                dialog, "workoutFirstPresetButton");
        QWidget *balanced = requiredChild<QWidget>(dialog, "balancedPresetButton");
        QWidget *rideFirst = requiredChild<QWidget>(dialog, "rideFirstPresetButton");
        QWidget *title = requiredChild<QWidget>(dialog, "courseTitleEdit");
        QWidget *output = requiredChild<QWidget>(dialog, "courseOutputPathEdit");
        QWidget *browse = requiredChild<QWidget>(dialog, "browseCourseOutputButton");
        QWidget *create = requiredChild<QWidget>(dialog, "createCourseButton");
        for (QWidget *widget : {heading, guarantee, workoutFirst, balanced, rideFirst,
                                title, output, browse, create}) {
            QVERIFY(dialog.rect().contains(dialogGeometry(widget)));
        }
        QVERIFY(dialog.rect().contains(dialogGeometry(scrollArea)));
        QVERIFY(dialog.rect().contains(dialogGeometry(buttonBox)));

        const int presetTop = dialogGeometry(workoutFirst).top();
        const int presetBottom = std::max({dialogGeometry(workoutFirst).bottom(),
                                          dialogGeometry(balanced).bottom(),
                                          dialogGeometry(rideFirst).bottom()});
        QVERIFY(dialogGeometry(heading).bottom()
                < dialogGeometry(guarantee).top());
        QVERIFY(dialogGeometry(guarantee).bottom() < presetTop);
        QVERIFY(presetBottom < dialogGeometry(scrollArea).top());
        QVERIFY(dialogGeometry(scrollArea).bottom() < dialogGeometry(title).top());
        QVERIFY(dialogGeometry(title).bottom() < dialogGeometry(output).top());
        QVERIFY(dialogGeometry(output).bottom() < dialogGeometry(buttonBox).top());
        QVERIFY(dialogGeometry(browse).intersects(dialogGeometry(output)) == false);
        QVERIFY(dialogGeometry(create).intersects(dialogGeometry(scrollArea)) == false);

        QVERIFY(scrollArea->widgetResizable());
        QCOMPARE(scrollArea->widget(), scrollContents);
        QVERIFY(scrollContents->width() <= scrollArea->viewport()->width());
        QVERIFY(scrollContents->height() > scrollArea->viewport()->height());
        QVERIFY(scrollArea->verticalScrollBar()->maximum() > 0);

        const char *scrollableControls[] = {
            "presetDescriptionLabel",
            "presetMetricsLabel",
            "workoutFirstComparisonValue",
            "balancedComparisonValue",
            "rideFirstComparisonValue",
            "coursePreview",
            "durationValue",
            "prescriptionChangesValue"
        };
        QWidget *previous = nullptr;
        for (const char *name : scrollableControls) {
            QWidget *control = requiredChild<QWidget>(dialog, name);
            if (previous) {
                const QRect previousGeometry(
                        previous->mapTo(scrollContents, QPoint(0, 0)),
                        previous->size());
                const QRect controlGeometry(
                        control->mapTo(scrollContents, QPoint(0, 0)),
                        control->size());
                QVERIFY2(previousGeometry.bottom() < controlGeometry.top(), name);
            }
            previous = control;
            scrollArea->ensureWidgetVisible(control, 0, 0);
            QCoreApplication::processEvents();
            const QRect viewportGeometry(
                    control->mapTo(scrollArea->viewport(), QPoint(0, 0)),
                    control->size());
            QVERIFY2(scrollArea->viewport()->rect().contains(viewportGeometry), name);
            if (QLabel *label = qobject_cast<QLabel *>(control)) {
                if (label->wordWrap()) {
                    const int requiredHeight = label->heightForWidth(label->width());
                    const QByteArray dimensions = QStringLiteral(
                            "%1: %2 px available, %3 px required at %4 px wide")
                            .arg(QLatin1String(name))
                            .arg(label->height())
                            .arg(requiredHeight)
                            .arg(label->width())
                            .toLatin1();
                    QVERIFY2(label->height() + 1 >= requiredHeight,
                             dimensions.constData());
                }
            }
        }
    }

private slots:
    void representativePresetsPreserveWorkoutAndIncreaseTrailCharacter()
    {
        WorkoutGameCourseSourceRequest request = sampleRequest();
        std::array<WorkoutGameCourseSourceResult, 3> results;
        for (std::size_t index = 0; index < results.size(); ++index) {
            request.preset = static_cast<WorkoutGameCoursePreset>(index);
            results[index] = WorkoutGameCourseSourceAdapter::convert(request);
            QCOMPARE(results[index].status, WorkoutGameCourseSourceStatus::Ready);
            QVERIFY(results[index].document.sourceIntervals.size() >= 10u);
            QCOMPARE(results[index].document.course.sections.size(),
                     results[index].document.sourceIntervals.size());
            for (std::size_t sectionIndex = 0;
                    sectionIndex < results[index].document.sourceIntervals.size();
                    ++sectionIndex) {
                const WorkoutGameInterval &source =
                        results[index].document.sourceIntervals[sectionIndex];
                const WorkoutGameDistanceCourseSection &generated =
                        results[index].document.course.sections[sectionIndex];
                QCOMPARE(generated.targetStartWatts, source.startWatts);
                QCOMPARE(generated.targetEndWatts, source.endWatts);
                QCOMPARE(generated.nominalDurationMs, source.durationMs);
                QCOMPARE(generated.minimumDurationMs, source.durationMs);
                QCOMPARE(generated.maximumDurationMs, source.durationMs);
            }
        }

        QVERIFY(results[0].summary.elevationGainMeters
                < results[1].summary.elevationGainMeters);
        QVERIFY(results[1].summary.elevationGainMeters
                < results[2].summary.elevationGainMeters);
        QVERIFY(results[0].summary.technicalTerrainExposurePercent
                < results[1].summary.technicalTerrainExposurePercent);
        QVERIFY(results[1].summary.technicalTerrainExposurePercent
                < results[2].summary.technicalTerrainExposurePercent);
        QVERIFY(totalAbsoluteTurn(results[0]) < totalAbsoluteTurn(results[1]));
        QVERIFY(totalAbsoluteTurn(results[1]) < totalAbsoluteTurn(results[2]));
    }

    void balancedPreviewShowsCompleteSummary()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        WorkoutGameCourseConversionDialog dialog(
                sampleRequest(), directory.filePath("intervals-mtb.crs"));
        dialog.resize(900, 700);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));

        QCOMPARE(dialog.selectedPreset(), WorkoutGameCoursePreset::Balanced);
        QCOMPARE(dialog.currentResult().status,
                 WorkoutGameCourseSourceStatus::Ready);
        QVERIFY(requiredChild<QToolButton>(dialog, "balancedPresetButton")
                        ->isChecked());
        QVERIFY(requiredChild<QLabel>(dialog, "prescriptionGuaranteeLabel")
                        ->text().contains("preserve", Qt::CaseInsensitive));
        QVERIFY(!requiredChild<QLabel>(dialog, "durationValue")->text().isEmpty());
        QVERIFY(requiredChild<QLabel>(dialog, "etaValue")->text().contains("-"));
        QVERIFY(requiredChild<QLabel>(dialog, "distanceValue")->text().contains("km"));
        QVERIFY(requiredChild<QLabel>(dialog, "ascentValue")->text().contains("m"));
        QVERIFY(requiredChild<QLabel>(dialog, "featuresValue")->text().contains("3"));
        QVERIFY(requiredChild<QLabel>(dialog, "loadValue")->text().contains("pts"));
        QVERIFY(requiredChild<QLabel>(dialog, "loadDeviationValue")->text().contains("%"));
        QVERIFY(requiredChild<QLabel>(dialog, "workDeviationValue")->text().contains("%"));
        QVERIFY(requiredChild<QLabel>(dialog, "recoveryDeviationValue")->text().contains("%"));
        QVERIFY(requiredChild<QLabel>(dialog, "totalDeviationValue")->text().contains("%"));
        QVERIFY(requiredChild<QLabel>(dialog, "keyEffortRetentionValue")
                        ->text().contains("/"));
        QVERIFY(requiredChild<QLabel>(dialog, "recoveryRetentionValue")
                        ->text().contains("/"));
        QVERIFY(!requiredChild<QLabel>(dialog, "terrainSignatureValue")
                        ->text().isEmpty());
        QVERIFY(requiredChild<QLabel>(dialog, "terrainSignatureValue")
                        ->text().contains("curve events", Qt::CaseInsensitive));
        QVERIFY(requiredChild<QLabel>(dialog, "technicalExposureValue")
                        ->text().contains("%"));
        QVERIFY(requiredChild<QLabel>(dialog, "featureDensityValue")
                        ->text().contains("/10 sections"));
        QCOMPARE(requiredChild<QLabel>(dialog, "runtimeExposureValue")->text(),
                 QStringLiteral("100% of prescribed interval time"));
        QVERIFY(!requiredChild<QLabel>(dialog, "prescriptionChangesValue")
                        ->text().isEmpty());
        QVERIFY(!requiredChild<QLabel>(dialog, "workoutFirstComparisonValue")
                        ->text().isEmpty());
        QVERIFY(!requiredChild<QLabel>(dialog, "balancedComparisonValue")
                        ->text().isEmpty());
        QVERIFY(!requiredChild<QLabel>(dialog, "rideFirstComparisonValue")
                        ->text().isEmpty());
        QVERIFY(requiredChild<QLabel>(dialog, "workoutFirstComparisonValue")
                        ->text()
                != requiredChild<QLabel>(dialog, "balancedComparisonValue")
                        ->text());
        QVERIFY(requiredChild<QLabel>(dialog, "balancedComparisonValue")
                        ->text()
                != requiredChild<QLabel>(dialog, "rideFirstComparisonValue")
                        ->text());
        for (const char *name : {
                "workoutFirstComparisonValue",
                "balancedComparisonValue",
                "rideFirstComparisonValue"}) {
            const QString text = requiredChild<QLabel>(dialog, name)->text();
            const QByteArray details = QStringLiteral("%1: %2")
                    .arg(QLatin1String(name), text).toLatin1();
            QVERIFY2(text.contains("key efforts", Qt::CaseInsensitive),
                     details.constData());
            QVERIFY2(text.contains(
                             "prescribed recoveries", Qt::CaseInsensitive),
                     details.constData());
        }
        QVERIFY(requiredChild<QPushButton>(dialog, "createCourseButton")
                        ->isEnabled());

        const QString screenshotPath = qEnvironmentVariable(
                "GC_MTB_DIALOG_SCREENSHOT");
        if (!screenshotPath.isEmpty()) {
            QImage screenshot(dialog.size(), QImage::Format_ARGB32_Premultiplied);
            screenshot.fill(Qt::transparent);
            dialog.render(&screenshot);
            QVERIFY2(screenshot.save(screenshotPath),
                     qPrintable(screenshotPath));
        }
    }

    void compactDialogDoesNotOverlapOrClip()
    {
        verifyResponsiveGeometry(1.0);
    }

    void largeFontDialogDoesNotOverlapOrClip()
    {
        verifyResponsiveGeometry(1.5);
    }

    void presetSwitchUpdatesPreviewButNotWorkoutTargets()
    {
        QTemporaryDir directory;
        const QString coursePath = directory.filePath("intervals-mtb.crs");
        WorkoutGameCourseConversionDialog dialog(
                sampleRequest(), coursePath);
        const WorkoutGameCourseSourceResult balanced = dialog.currentResult();
        QVERIFY(!QFileInfo::exists(coursePath));
        QVERIFY(!QFileInfo::exists(
                WorkoutGameCourseDocumentStore::sidecarPathForCourse(
                    coursePath)));

        requiredChild<QToolButton>(dialog, "rideFirstPresetButton")->click();
        const WorkoutGameCourseSourceResult rideFirst = dialog.currentResult();

        QCOMPARE(dialog.selectedPreset(), WorkoutGameCoursePreset::RideFirst);
        QVERIFY(requiredChild<QLabel>(dialog, "presetDescriptionLabel")
                        ->text().contains("ride first",
                                         Qt::CaseInsensitive));
        QVERIFY(requiredChild<QLabel>(dialog, "presetMetricsLabel")
                        ->text().contains("1.30x", Qt::CaseInsensitive));
        QVERIFY(requiredChild<QLabel>(dialog, "presetMetricsLabel")
                        ->text().contains("total turn", Qt::CaseInsensitive));
        QCOMPARE(requiredChild<QLabel>(dialog, "runtimeExposureValue")->text(),
                 QStringLiteral("100% of prescribed interval time"));
        QCOMPARE(rideFirst.status, WorkoutGameCourseSourceStatus::Ready);
        QVERIFY(rideFirst.summary.elevationGainMeters
                > balanced.summary.elevationGainMeters);
        QCOMPARE(rideFirst.summary.nominalDurationMs,
                 balanced.summary.nominalDurationMs);
        QCOMPARE(rideFirst.summary.estimatedLoadPoints,
                 balanced.summary.estimatedLoadPoints);
        QCOMPARE(rideFirst.summary.workDurationDeviationPercent, 0.0);
        QCOMPARE(rideFirst.summary.recoveryDurationDeviationPercent, 0.0);
        QVERIFY(rideFirst.summary.technicalTerrainExposurePercent
                >= balanced.summary.technicalTerrainExposurePercent);
        QVERIFY(rideFirst.summary.technicalFeatureDensityPerTenSections
                >= balanced.summary.technicalFeatureDensityPerTenSections);
        QVERIFY(totalAbsoluteTurn(rideFirst) > totalAbsoluteTurn(balanced));
        QCOMPARE(rideFirst.document.course.sections.size(),
                 balanced.document.course.sections.size());
        for (std::size_t index = 0;
                index < balanced.document.course.sections.size();
                ++index) {
            QCOMPARE(rideFirst.document.course.sections[index].targetStartWatts,
                     balanced.document.course.sections[index].targetStartWatts);
            QCOMPARE(rideFirst.document.course.sections[index].targetEndWatts,
                     balanced.document.course.sections[index].targetEndWatts);
            QCOMPARE(rideFirst.document.course.sections[index].nominalDurationMs,
                     balanced.document.course.sections[index].nominalDurationMs);
        }
        QVERIFY(!QFileInfo::exists(coursePath));
        QVERIFY(!QFileInfo::exists(
                WorkoutGameCourseDocumentStore::sidecarPathForCourse(
                    coursePath)));

        requiredChild<QToolButton>(dialog, "balancedPresetButton")->click();
        const WorkoutGameCourseSourceResult repeated = dialog.currentResult();
        QCOMPARE(repeated.summary.nominalDurationMs,
                 balanced.summary.nominalDurationMs);
        QCOMPARE(repeated.summary.distanceMeters,
                 balanced.summary.distanceMeters);
        QCOMPARE(WorkoutGameCourseDocumentCodec::encode(repeated.document),
                 WorkoutGameCourseDocumentCodec::encode(balanced.document));
        QVERIFY(!QFileInfo::exists(coursePath));
        QVERIFY(!QFileInfo::exists(
                WorkoutGameCourseDocumentStore::sidecarPathForCourse(
                    coursePath)));
    }

    void presetsSharePrescriptionGuaranteeAndDescribeDistinctTrailCharacter()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        WorkoutGameCourseConversionDialog dialog(
                sampleRequest(), directory.filePath("intervals-mtb.crs"));

        const QString guarantee = requiredChild<QLabel>(
                dialog, "prescriptionGuaranteeLabel")->text();
        QVERIFY(guarantee.contains("all presets", Qt::CaseInsensitive));
        QVERIFY(guarantee.contains("target", Qt::CaseInsensitive));
        QVERIFY(guarantee.contains("timing", Qt::CaseInsensitive));

        struct ExpectedPreset {
            const char *button;
            const char *character;
        };
        const ExpectedPreset expected[] = {
            {"workoutFirstPresetButton", "workout first"},
            {"balancedPresetButton", "balanced"},
            {"rideFirstPresetButton", "ride first"}
        };
        QStringList descriptions;
        for (const ExpectedPreset &preset : expected) {
            requiredChild<QToolButton>(dialog, preset.button)->click();
            const QString description = requiredChild<QLabel>(
                    dialog, "presetDescriptionLabel")->text();
            descriptions.append(description);
            QVERIFY2(description.startsWith(
                             QLatin1String(preset.character),
                             Qt::CaseInsensitive),
                     qPrintable(description));
            QVERIFY2(description.contains("prescribed", Qt::CaseInsensitive),
                     qPrintable(description));
            QVERIFY(description.contains("interval times", Qt::CaseInsensitive));
            const QString metrics = requiredChild<QLabel>(
                    dialog, "presetMetricsLabel")->text();
            QVERIFY(metrics.contains("grade", Qt::CaseInsensitive));
            QVERIFY(metrics.contains("curvature", Qt::CaseInsensitive));
            QVERIFY(metrics.contains("technical", Qt::CaseInsensitive));
            QVERIFY(metrics.contains("ascent", Qt::CaseInsensitive));
            QVERIFY(metrics.contains("total turn", Qt::CaseInsensitive));
        }
        QCOMPARE(descriptions.size(), 3);
        QVERIFY(descriptions[0] != descriptions[1]);
        QVERIFY(descriptions[1] != descriptions[2]);
        QVERIFY(descriptions[0] != descriptions[2]);
    }

    void compactComparisonIsVisibleAboveScrollableDetails()
    {
        QTemporaryDir directory;
        WorkoutGameCourseConversionDialog dialog(
                sampleRequest(), directory.filePath("visible-comparison.crs"));
        dialog.resize(900, 700);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QCoreApplication::processEvents();

        QWidget *comparison = requiredChild<QWidget>(
                dialog, "courseModeComparison");
        QScrollArea *scroll = requiredChild<QScrollArea>(
                dialog, "courseDetailsScrollArea");
        QVERIFY(comparison->isVisibleTo(&dialog));
        const QRect comparisonGeometry(
                comparison->mapTo(&dialog, QPoint(0, 0)), comparison->size());
        const QRect viewportGeometry(
                scroll->viewport()->mapTo(&dialog, QPoint(0, 0)),
                scroll->viewport()->size());
        QVERIFY(viewportGeometry.contains(comparisonGeometry));
        QCOMPARE(scroll->verticalScrollBar()->value(), 0);
    }

    void workoutPowerProfileUsesSourceTimeNotGeneratedDistance()
    {
        const std::vector<WorkoutGameInterval> unevenDurations {
            {0, 60000, 100.0, 100.0},
            {60000, 180000, 200.0, 200.0}
        };
        const std::vector<WorkoutGameCoursePreviewPoint> unevenPower =
                WorkoutGameCoursePreviewMetrics::workoutPowerProfile(
                    unevenDurations);
        QCOMPARE(unevenPower.size(), std::size_t(4));
        QCOMPARE(unevenPower[0].progress, 0.0);
        QCOMPARE(unevenPower[1].progress, 0.25);
        QCOMPARE(unevenPower[2].progress, 0.25);
        QCOMPARE(unevenPower[3].progress, 1.0);

        WorkoutGameCourseSourceRequest request = sampleRequest();
        const WorkoutGameCourseSourceResult balanced =
                WorkoutGameCourseSourceAdapter::convert(request);
        request.preset = WorkoutGameCoursePreset::RideFirst;
        const WorkoutGameCourseSourceResult rideFirst =
                WorkoutGameCourseSourceAdapter::convert(request);
        QCOMPARE(balanced.status, WorkoutGameCourseSourceStatus::Ready);
        QCOMPARE(rideFirst.status, WorkoutGameCourseSourceStatus::Ready);

        const std::vector<WorkoutGameCoursePreviewPoint> balancedPower =
                WorkoutGameCoursePreviewMetrics::workoutPowerProfile(
                    balanced.document.sourceIntervals);
        const std::vector<WorkoutGameCoursePreviewPoint> rideFirstPower =
                WorkoutGameCoursePreviewMetrics::workoutPowerProfile(
                    rideFirst.document.sourceIntervals);
        QCOMPARE(balancedPower.size(), rideFirstPower.size());
        QVERIFY(!balancedPower.empty());
        for (std::size_t index = 0; index < balancedPower.size(); ++index) {
            QCOMPARE(balancedPower[index].progress, rideFirstPower[index].progress);
            QCOMPARE(balancedPower[index].value, rideFirstPower[index].value);
        }
        QCOMPARE(balancedPower.front().progress, 0.0);
        QCOMPARE(balancedPower.back().progress, 1.0);
    }

    void roadPiecesAreGroupedIntoCoherentCurveEvents()
    {
        WorkoutGameRoadPlan plan;
        auto appendPiece = [&](std::size_t section, double degrees) {
            WorkoutGameRoadPiece piece;
            piece.sourceSectionIndex = section;
            piece.lengthMeters = 20.0;
            piece.turnRadians = degrees * 3.14159265358979323846 / 180.0;
            plan.pieces.push_back(piece);
        };
        appendPiece(0, 8.0);
        appendPiece(0, -9.0);
        appendPiece(0, 7.0);
        appendPiece(1, 4.0);
        appendPiece(1, 3.0);
        appendPiece(2, 13.0);

        const WorkoutGameCoursePreviewRoadMetrics metrics =
                WorkoutGameCoursePreviewMetrics::roadMetrics(plan);
        QCOMPARE(metrics.curveEventCount, 2);
        QCOMPARE(metrics.roadPieceCount, 6);
    }

    void previewWidgetRendersElevationAndPowerData()
    {
        const WorkoutGameCourseSourceResult result =
                WorkoutGameCourseSourceAdapter::convert(sampleRequest());
        QCOMPARE(result.status, WorkoutGameCourseSourceStatus::Ready);
        WorkoutGameCoursePreviewWidget preview;
        preview.resize(820, 300);
        preview.setResult(result);

        QImage image(preview.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        preview.render(&image);

        std::set<QRgb> colors;
        int opaquePixels = 0;
        for (int y = 0; y < image.height(); y += 2) {
            const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
            for (int x = 0; x < image.width(); x += 2) {
                colors.insert(line[x]);
                if (qAlpha(line[x]) == 255) ++opaquePixels;
            }
        }
        QVERIFY(opaquePixels > 20000);
        QVERIFY(colors.size() > 20);
    }

    void createWritesNewPairAndAcceptsDialog()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString coursePath = directory.filePath("new-course.crs");
        WorkoutGameCourseConversionDialog dialog(sampleRequest(), coursePath);

        requiredChild<QLineEdit>(dialog, "courseTitleEdit")
                ->setText(QStringLiteral("Thursday MTB"));
        requiredChild<QPushButton>(dialog, "createCourseButton")->click();

        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(dialog.generatedCoursePath(), coursePath);
        QVERIFY(QFileInfo(coursePath).isFile());
        QVERIFY(QFileInfo(
                WorkoutGameCourseDocumentStore::sidecarPathForCourse(coursePath))
                        .isFile());
        WorkoutGameCourseDocument loaded;
        QString error;
        QCOMPARE(WorkoutGameCourseDocumentStore::loadForCourse(
                    coursePath, loaded, error),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(loaded.title, QStringLiteral("Thursday MTB"));
    }

    void createPersistsExactPreviewApartFromEditedTitle()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString coursePath = directory.filePath("exact-preview.crs");
        WorkoutGameCourseConversionDialog dialog(sampleRequest(), coursePath);
        WorkoutGameCourseDocument expected = dialog.currentResult().document;
        expected.title = QStringLiteral("Exact preview title");

        requiredChild<QLineEdit>(dialog, "courseTitleEdit")
                ->setText(expected.title);
        requiredChild<QPushButton>(dialog, "createCourseButton")->click();

        WorkoutGameCourseDocument loaded;
        QString error;
        QCOMPARE(WorkoutGameCourseDocumentStore::loadForCourse(
                    coursePath, loaded, error),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(WorkoutGameCourseDocumentCodec::encode(loaded),
                 WorkoutGameCourseDocumentCodec::encode(expected));
    }

    void conflictLeavesDialogOpenAndShowsError()
    {
        QTemporaryDir directory;
        const QString coursePath = directory.filePath("existing.crs");
        WorkoutGameCourseConversionDialog first(sampleRequest(), coursePath);
        requiredChild<QPushButton>(first, "createCourseButton")->click();
        QCOMPARE(first.result(), int(QDialog::Accepted));

        WorkoutGameCourseConversionDialog second(sampleRequest(), coursePath);
        requiredChild<QPushButton>(second, "createCourseButton")->click();

        QCOMPARE(second.result(), 0);
        QLabel *error = requiredChild<QLabel>(second, "courseErrorLabel");
        QVERIFY(error->isVisibleTo(&second) || !error->isHidden());
        QVERIFY(!error->text().isEmpty());
    }

    void editRegeneratesAndReplacesExistingCourse()
    {
        QTemporaryDir directory;
        const QString coursePath = directory.filePath("editable.crs");
        const WorkoutGameCourseSourceResult source =
                WorkoutGameCourseSourceAdapter::convert(sampleRequest());
        QString error;
        QCOMPARE(WorkoutGameCourseDocumentStore::saveNewArtifact(
                    coursePath, source.document, error),
                 WorkoutGameCourseDocumentStatus::Ready);

        WorkoutGameCourseConversionDialog dialog(
                source.document, coursePath);
        QCOMPARE(dialog.selectedPreset(), WorkoutGameCoursePreset::Balanced);
        QVERIFY(requiredChild<QLineEdit>(dialog, "courseOutputPathEdit")
                        ->isReadOnly());
        requiredChild<QToolButton>(dialog, "rideFirstPresetButton")->click();
        requiredChild<QLineEdit>(dialog, "courseTitleEdit")
                ->setText(QStringLiteral("Edited MTB"));
        requiredChild<QPushButton>(dialog, "createCourseButton")->click();

        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        WorkoutGameCourseDocument loaded;
        QCOMPARE(WorkoutGameCourseDocumentStore::loadForCourse(
                    coursePath, loaded, error),
                 WorkoutGameCourseDocumentStatus::Ready);
        QCOMPARE(loaded.title, QStringLiteral("Edited MTB"));
        QCOMPARE(loaded.preset, WorkoutGameCoursePreset::RideFirst);
        QVERIFY(!loaded.sourceIntervals.empty());
    }
};

QTEST_MAIN(TestWorkoutGameCourseConversionDialog)
#include "testWorkoutGameCourseConversionDialog.moc"
