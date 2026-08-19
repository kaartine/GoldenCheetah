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

#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRect>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

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
    append(8 * 60000.0, 135.0);
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

private slots:
    void balancedPreviewShowsCompleteSummary()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        WorkoutGameCourseConversionDialog dialog(
                sampleRequest(), directory.filePath("intervals-mtb.crs"));
        dialog.resize(900, 680);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));

        QCOMPARE(dialog.selectedPreset(), WorkoutGameCoursePreset::Balanced);
        QCOMPARE(dialog.currentResult().status,
                 WorkoutGameCourseSourceStatus::Ready);
        QVERIFY(requiredChild<QToolButton>(dialog, "balancedPresetButton")
                        ->isChecked());
        QVERIFY(!requiredChild<QLabel>(dialog, "durationValue")->text().isEmpty());
        QVERIFY(requiredChild<QLabel>(dialog, "etaValue")->text().contains("-"));
        QVERIFY(requiredChild<QLabel>(dialog, "distanceValue")->text().contains("km"));
        QVERIFY(requiredChild<QLabel>(dialog, "ascentValue")->text().contains("m"));
        QVERIFY(requiredChild<QLabel>(dialog, "featuresValue")->text().contains("3"));
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

    void minimumSizeKeepsCriticalControlsInsideDialog()
    {
        QTemporaryDir directory;
        WorkoutGameCourseConversionDialog dialog(
                sampleRequest(), directory.filePath("intervals-mtb.crs"));
        dialog.resize(dialog.minimumSize());
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));

        const QRect available = dialog.rect();
        const char *criticalControls[] = {
            "workoutFirstPresetButton",
            "balancedPresetButton",
            "rideFirstPresetButton",
            "coursePreview",
            "courseTitleEdit",
            "courseOutputPathEdit",
            "browseCourseOutputButton",
            "createCourseButton"
        };
        for (const char *name : criticalControls) {
            QWidget *control = requiredChild<QWidget>(dialog, name);
            const QRect geometry(
                    control->mapTo(&dialog, QPoint(0, 0)), control->size());
            QVERIFY2(available.contains(geometry), name);
            QVERIFY2(control->width() >= control->minimumSizeHint().width()
                        || control->sizePolicy().horizontalPolicy()
                            == QSizePolicy::Expanding,
                     name);
        }
    }

    void presetSwitchUpdatesPreviewButNotWorkoutTargets()
    {
        QTemporaryDir directory;
        WorkoutGameCourseConversionDialog dialog(
                sampleRequest(), directory.filePath("intervals-mtb.crs"));
        const WorkoutGameCourseSourceResult balanced = dialog.currentResult();

        requiredChild<QToolButton>(dialog, "rideFirstPresetButton")->click();
        const WorkoutGameCourseSourceResult rideFirst = dialog.currentResult();

        QCOMPARE(dialog.selectedPreset(), WorkoutGameCoursePreset::RideFirst);
        QCOMPARE(rideFirst.status, WorkoutGameCourseSourceStatus::Ready);
        QVERIFY(rideFirst.summary.elevationGainMeters
                > balanced.summary.elevationGainMeters);
        QCOMPARE(rideFirst.document.course.sections.size(),
                 balanced.document.course.sections.size());
        for (std::size_t index = 0;
                index < balanced.document.course.sections.size();
                ++index) {
            QCOMPARE(rideFirst.document.course.sections[index].targetStartWatts,
                     balanced.document.course.sections[index].targetStartWatts);
            QCOMPARE(rideFirst.document.course.sections[index].targetEndWatts,
                     balanced.document.course.sections[index].targetEndWatts);
        }
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
