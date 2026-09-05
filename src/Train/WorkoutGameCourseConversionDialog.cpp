/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "WorkoutGameCourseConversionDialog.h"

#include "WorkoutGameCoursePreviewWidget.h"

#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStringList>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace {

QString durationText(std::int64_t durationMs)
{
    const std::int64_t totalSeconds = (durationMs + 500) / 1000;
    const std::int64_t hours = totalSeconds / 3600;
    const std::int64_t minutes = totalSeconds / 60 % 60;
    const std::int64_t seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2:%3")
            .arg(hours, 2, 10, QLatin1Char('0'))
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
}

QLabel *summaryValue(const char *objectName, QWidget *parent)
{
    QLabel *label = new QLabel(parent);
    label->setObjectName(QLatin1String(objectName));
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

std::size_t presetIndex(WorkoutGameCoursePreset preset)
{
    switch (preset) {
    case WorkoutGameCoursePreset::WorkoutFirst: return 0u;
    case WorkoutGameCoursePreset::Balanced: return 1u;
    case WorkoutGameCoursePreset::RideFirst: return 2u;
    }
    return 1u;
}

QString percentText(double value)
{
    return QStringLiteral("%1%").arg(value, 0, 'f', 1);
}

QString terrainSignatureText(const WorkoutGameCourseSourceResult &result)
{
    if (result.status != WorkoutGameCourseSourceStatus::Ready) return {};
    return QStringLiteral("grade %1 | +%2 m | %3 technical / %4 total features")
            .arg(result.document.generationParameters.gradeScale, 0, 'f', 2)
            .arg(std::lround(result.summary.elevationGainMeters))
            .arg(result.summary.technicalFeatureCount)
            .arg(qulonglong(result.document.course.sections.size()));
}

QString technicalExposureText(
        const WorkoutGameCourseConversionSummary &summary)
{
    return summary.technicalTerrainExposureApplicable
            ? percentText(summary.technicalTerrainExposurePercent)
            : QStringLiteral("N/A");
}

QString prescriptionChangesText(
        const WorkoutGameCourseConversionSummary &summary)
{
    if (summary.prescriptionChanges.empty()) {
        return QStringLiteral("No prescription changes");
    }
    QStringList values;
    for (const auto &change : summary.prescriptionChanges) {
        QString role;
        switch (change.role) {
        case WorkoutGameCourseIntervalRole::Prescribed:
            role = QStringLiteral("prescribed"); break;
        case WorkoutGameCourseIntervalRole::NonPrescriptiveWarmup:
            role = QStringLiteral("non-prescriptive warmup"); break;
        case WorkoutGameCourseIntervalRole::NonPrescriptiveCooldown:
            role = QStringLiteral("non-prescriptive cooldown"); break;
        case WorkoutGameCourseIntervalRole::NonPrescriptiveTransition:
            role = QStringLiteral("non-prescriptive transition"); break;
        }
        values.append(QStringLiteral("#%1 %2: %3 ms")
                .arg(qulonglong(change.intervalIndex + 1))
                .arg(role)
                .arg(change.generatedDurationMs - change.sourceDurationMs));
    }
    return values.join(QStringLiteral("; "));
}

QString comparisonText(const WorkoutGameCourseSourceResult &result)
{
    if (result.status != WorkoutGameCourseSourceStatus::Ready) {
        return QStringLiteral("Unavailable");
    }
    const WorkoutGameCourseConversionSummary &value = result.summary;
    return QStringLiteral(
            "%1 (%2 total) | %3 km | %4 pts (%5 load) | "
            "key %6/%7 | recovery %8/%9 | work %10 | recovery/rest %11 | "
            "technical %12, %13/10 | curvature %14 deg/100 m | %15 | %16")
            .arg(durationText(value.nominalDurationMs))
            .arg(percentText(value.totalDurationDeviationPercent))
            .arg(value.distanceMeters / 1000.0, 0, 'f', 1)
            .arg(value.estimatedLoadPoints, 0, 'f', 1)
            .arg(percentText(value.loadDeviationPercent))
            .arg(value.preservedKeyEffortCount)
            .arg(value.keyEffortCount)
            .arg(value.preservedRecoveryCount)
            .arg(value.recoveryCount)
            .arg(percentText(value.workDurationDeviationPercent))
            .arg(percentText(value.recoveryDurationDeviationPercent))
            .arg(technicalExposureText(value))
            .arg(value.technicalFeatureDensityPerTenSections, 0, 'f', 1)
            .arg(value.curvatureDegreesPer100m, 0, 'f', 1)
            .arg(terrainSignatureText(result))
            .arg(prescriptionChangesText(value));
}

}

WorkoutGameCourseConversionDialog::WorkoutGameCourseConversionDialog(
        const WorkoutGameCourseSourceRequest &request,
        const QString &defaultCoursePath,
        QWidget *parent)
    : QDialog(parent), sourceRequest(request)
{
    setObjectName(QStringLiteral("workoutGameCourseConversionDialog"));
    setWindowTitle(tr("Create MTB Course"));
    setModal(true);
    resize(1040, 820);
    setMinimumSize(820, 700);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(14);

    QLabel *heading = new QLabel(tr("Create MTB Course"), this);
    heading->setObjectName(QStringLiteral("courseDialogHeading"));
    QFont headingFont = heading->font();
    headingFont.setPointSize(headingFont.pointSize() + 3);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    layout->addWidget(heading);

    QHBoxLayout *presetLayout = new QHBoxLayout;
    presetLayout->setSpacing(0);
    QButtonGroup *presetGroup = new QButtonGroup(this);
    presetGroup->setExclusive(true);
    auto addPreset = [&](const QString &text, const char *name) {
        QToolButton *button = new QToolButton(this);
        button->setText(text);
        button->setObjectName(QLatin1String(name));
        button->setCheckable(true);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setMinimumHeight(34);
        presetGroup->addButton(button);
        presetLayout->addWidget(button);
        return button;
    };
    workoutFirstButton = addPreset(
            tr("Workout first"), "workoutFirstPresetButton");
    balancedButton = addPreset(tr("Balanced"), "balancedPresetButton");
    rideFirstButton = addPreset(tr("Ride first"), "rideFirstPresetButton");
    balancedButton->setChecked(true);
    const QString segmentStyle = QStringLiteral(
            "QToolButton { border: 1px solid palette(mid); padding: 6px 12px; }"
            "QToolButton:checked { background: palette(highlight);"
            " color: palette(highlighted-text); }");
    workoutFirstButton->setStyleSheet(segmentStyle);
    balancedButton->setStyleSheet(segmentStyle);
    rideFirstButton->setStyleSheet(segmentStyle);
    connect(workoutFirstButton, &QToolButton::clicked,
            this, &WorkoutGameCourseConversionDialog::selectWorkoutFirst);
    connect(balancedButton, &QToolButton::clicked,
            this, &WorkoutGameCourseConversionDialog::selectBalanced);
    connect(rideFirstButton, &QToolButton::clicked,
            this, &WorkoutGameCourseConversionDialog::selectRideFirst);
    layout->addLayout(presetLayout);

    preview = new WorkoutGameCoursePreviewWidget(this);
    layout->addWidget(preview, 1);

    QGridLayout *summary = new QGridLayout;
    summary->setHorizontalSpacing(18);
    summary->setVerticalSpacing(5);
    durationValue = summaryValue("durationValue", this);
    etaValue = summaryValue("etaValue", this);
    distanceValue = summaryValue("distanceValue", this);
    ascentValue = summaryValue("ascentValue", this);
    featuresValue = summaryValue("featuresValue", this);
    loadValue = summaryValue("loadValue", this);
    loadDeviationValue = summaryValue("loadDeviationValue", this);
    workDeviationValue = summaryValue("workDeviationValue", this);
    recoveryDeviationValue = summaryValue("recoveryDeviationValue", this);
    totalDeviationValue = summaryValue("totalDeviationValue", this);
    keyEffortRetentionValue = summaryValue("keyEffortRetentionValue", this);
    recoveryRetentionValue = summaryValue("recoveryRetentionValue", this);
    terrainSignatureValue = summaryValue("terrainSignatureValue", this);
    technicalExposureValue = summaryValue("technicalExposureValue", this);
    featureDensityValue = summaryValue("featureDensityValue", this);
    curvatureValue = summaryValue("curvatureValue", this);
    prescriptionChangesValue = summaryValue("prescriptionChangesValue", this);
    summary->addWidget(new QLabel(tr("Workout duration"), this), 0, 0);
    summary->addWidget(durationValue, 0, 1);
    summary->addWidget(new QLabel(tr("Expected ride time"), this), 0, 2);
    summary->addWidget(etaValue, 0, 3);
    summary->addWidget(new QLabel(tr("Distance"), this), 1, 0);
    summary->addWidget(distanceValue, 1, 1);
    summary->addWidget(new QLabel(tr("Ascent"), this), 1, 2);
    summary->addWidget(ascentValue, 1, 3);
    summary->addWidget(new QLabel(tr("Features"), this), 2, 0);
    summary->addWidget(featuresValue, 2, 1, 1, 3);
    summary->addWidget(new QLabel(tr("Load"), this), 3, 0);
    summary->addWidget(loadValue, 3, 1);
    summary->addWidget(new QLabel(tr("Load deviation"), this), 3, 2);
    summary->addWidget(loadDeviationValue, 3, 3);
    summary->addWidget(new QLabel(tr("Key efforts preserved"), this), 4, 0);
    summary->addWidget(keyEffortRetentionValue, 4, 1);
    summary->addWidget(new QLabel(tr("Recoveries preserved"), this), 4, 2);
    summary->addWidget(recoveryRetentionValue, 4, 3);
    summary->addWidget(new QLabel(tr("Work deviation"), this), 5, 0);
    summary->addWidget(workDeviationValue, 5, 1);
    summary->addWidget(new QLabel(tr("Recovery/rest deviation"), this), 5, 2);
    summary->addWidget(recoveryDeviationValue, 5, 3);
    summary->addWidget(new QLabel(tr("Total deviation"), this), 6, 0);
    summary->addWidget(totalDeviationValue, 6, 1);
    summary->addWidget(new QLabel(tr("Terrain signature"), this), 6, 2);
    summary->addWidget(terrainSignatureValue, 6, 3);
    summary->addWidget(new QLabel(tr("Technical exposure"), this), 7, 0);
    summary->addWidget(technicalExposureValue, 7, 1);
    summary->addWidget(new QLabel(tr("Feature density"), this), 7, 2);
    summary->addWidget(featureDensityValue, 7, 3);
    summary->addWidget(new QLabel(tr("Curvature"), this), 8, 0);
    summary->addWidget(curvatureValue, 8, 1);
    summary->addWidget(new QLabel(tr("Prescription changes"), this), 8, 2);
    summary->addWidget(prescriptionChangesValue, 8, 3);
    summary->setColumnStretch(1, 1);
    summary->setColumnStretch(3, 1);
    layout->addLayout(summary);

    QGridLayout *comparison = new QGridLayout;
    workoutFirstComparisonValue = summaryValue(
            "workoutFirstComparisonValue", this);
    balancedComparisonValue = summaryValue("balancedComparisonValue", this);
    rideFirstComparisonValue = summaryValue("rideFirstComparisonValue", this);
    for (QLabel *label : {workoutFirstComparisonValue,
                          balancedComparisonValue,
                          rideFirstComparisonValue}) {
        label->setWordWrap(true);
    }
    comparison->addWidget(new QLabel(tr("Workout first"), this), 0, 0);
    comparison->addWidget(workoutFirstComparisonValue, 0, 1);
    comparison->addWidget(new QLabel(tr("Balanced"), this), 1, 0);
    comparison->addWidget(balancedComparisonValue, 1, 1);
    comparison->addWidget(new QLabel(tr("Ride first"), this), 2, 0);
    comparison->addWidget(rideFirstComparisonValue, 2, 1);
    comparison->setColumnStretch(1, 1);
    layout->addLayout(comparison);

    QGridLayout *fields = new QGridLayout;
    fields->setHorizontalSpacing(10);
    fields->setVerticalSpacing(7);
    titleEdit = new QLineEdit(this);
    titleEdit->setObjectName(QStringLiteral("courseTitleEdit"));
    outputPathEdit = new QLineEdit(defaultCoursePath, this);
    outputPathEdit->setObjectName(QStringLiteral("courseOutputPathEdit"));
    QPushButton *browse = new QPushButton(this);
    browse->setObjectName(QStringLiteral("browseCourseOutputButton"));
    browse->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
    browse->setToolTip(tr("Choose course file"));
    browse->setAccessibleName(tr("Choose course file"));
    browse->setFixedWidth(38);
    connect(browse, &QPushButton::clicked,
            this, &WorkoutGameCourseConversionDialog::browseOutput);
    fields->addWidget(new QLabel(tr("Title"), this), 0, 0);
    fields->addWidget(titleEdit, 0, 1, 1, 2);
    fields->addWidget(new QLabel(tr("Course file"), this), 1, 0);
    fields->addWidget(outputPathEdit, 1, 1);
    fields->addWidget(browse, 1, 2);
    fields->setColumnStretch(1, 1);
    layout->addLayout(fields);

    errorLabel = new QLabel(this);
    errorLabel->setObjectName(QStringLiteral("courseErrorLabel"));
    errorLabel->setWordWrap(true);
    QPalette errorPalette = errorLabel->palette();
    errorPalette.setColor(QPalette::WindowText, QColor(180, 45, 45));
    errorLabel->setPalette(errorPalette);
    errorLabel->hide();
    layout->addWidget(errorLabel);

    QDialogButtonBox *buttons = new QDialogButtonBox(this);
    createButton = buttons->addButton(
            tr("Create Course"), QDialogButtonBox::AcceptRole);
    createButton->setObjectName(QStringLiteral("createCourseButton"));
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(createButton, &QPushButton::clicked,
            this, &WorkoutGameCourseConversionDialog::createCourse);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    generatePreviews();
    selectPreset(WorkoutGameCoursePreset::Balanced);
    if (previewResult.status == WorkoutGameCourseSourceStatus::Ready) {
        titleEdit->setText(previewResult.document.title);
    }
}

WorkoutGameCourseConversionDialog::WorkoutGameCourseConversionDialog(
        const WorkoutGameCourseDocument &document,
        const QString &coursePath,
        QWidget *parent)
    : WorkoutGameCourseConversionDialog(
        WorkoutGameCourseSourceRequest(), coursePath, parent)
{
    editMode = true;
    editSourceDocument = document;
    setWindowTitle(tr("Edit MTB Course"));
    if (QLabel *heading = findChild<QLabel *>(
                QStringLiteral("courseDialogHeading"))) {
        heading->setText(tr("Edit MTB Course"));
    }
    titleEdit->setText(document.title);
    outputPathEdit->setReadOnly(true);
    if (QPushButton *browse = findChild<QPushButton *>(
                QStringLiteral("browseCourseOutputButton"))) {
        browse->setEnabled(false);
    }
    createButton->setText(tr("Save Course"));
    generatePreviews();
    selectPreset(document.preset);
}

WorkoutGameCoursePreset WorkoutGameCourseConversionDialog::selectedPreset() const
{
    return preset;
}

const WorkoutGameCourseSourceResult &
WorkoutGameCourseConversionDialog::currentResult() const
{
    return previewResult;
}

QString WorkoutGameCourseConversionDialog::generatedCoursePath() const
{
    return createdPath;
}

void WorkoutGameCourseConversionDialog::selectWorkoutFirst()
{
    selectPreset(WorkoutGameCoursePreset::WorkoutFirst);
}

void WorkoutGameCourseConversionDialog::selectBalanced()
{
    selectPreset(WorkoutGameCoursePreset::Balanced);
}

void WorkoutGameCourseConversionDialog::selectRideFirst()
{
    selectPreset(WorkoutGameCoursePreset::RideFirst);
}

void WorkoutGameCourseConversionDialog::selectPreset(
        WorkoutGameCoursePreset selected)
{
    preset = selected;
    workoutFirstButton->setChecked(preset == WorkoutGameCoursePreset::WorkoutFirst);
    balancedButton->setChecked(preset == WorkoutGameCoursePreset::Balanced);
    rideFirstButton->setChecked(preset == WorkoutGameCoursePreset::RideFirst);
    previewResult = modePreviews[presetIndex(preset)];
    preview->setResult(previewResult);
    refreshSummary();
}

void WorkoutGameCourseConversionDialog::generatePreviews()
{
    const WorkoutGameCoursePreset presets[] = {
        WorkoutGameCoursePreset::WorkoutFirst,
        WorkoutGameCoursePreset::Balanced,
        WorkoutGameCoursePreset::RideFirst
    };
    for (WorkoutGameCoursePreset mode : presets) {
        if (editMode) {
            modePreviews[presetIndex(mode)] =
                    WorkoutGameCourseSourceAdapter::regenerate(
                        editSourceDocument, mode, editSourceDocument.title);
        } else {
            WorkoutGameCourseSourceRequest request = sourceRequest;
            request.preset = mode;
            modePreviews[presetIndex(mode)] =
                    WorkoutGameCourseSourceAdapter::convert(request);
        }
    }
}

void WorkoutGameCourseConversionDialog::refreshSummary()
{
    const bool ready = previewResult.status == WorkoutGameCourseSourceStatus::Ready;
    createButton->setEnabled(ready);
    if (!ready) {
        durationValue->clear();
        etaValue->clear();
        distanceValue->clear();
        ascentValue->clear();
        featuresValue->clear();
        loadValue->clear();
        loadDeviationValue->clear();
        workDeviationValue->clear();
        recoveryDeviationValue->clear();
        totalDeviationValue->clear();
        keyEffortRetentionValue->clear();
        recoveryRetentionValue->clear();
        terrainSignatureValue->clear();
        technicalExposureValue->clear();
        featureDensityValue->clear();
        curvatureValue->clear();
        prescriptionChangesValue->clear();
        showError(tr("This workout cannot be converted to an MTB course."));
        return;
    }
    errorLabel->hide();
    const WorkoutGameCourseConversionSummary &value = previewResult.summary;
    durationValue->setText(durationText(value.nominalDurationMs));
    etaValue->setText(QStringLiteral("%1 - %2")
            .arg(durationText(value.fastEstimate.elapsedTimeMs),
                 durationText(value.slowEstimate.elapsedTimeMs)));
    distanceValue->setText(QStringLiteral("%1 km")
            .arg(value.distanceMeters / 1000.0, 0, 'f', 1));
    ascentValue->setText(QStringLiteral("%1 m")
            .arg(std::lround(value.elevationGainMeters)));
    featuresValue->setText(tr("%1 technical | %2 climbs | %3 jumps | %4 descents")
            .arg(value.technicalFeatureCount)
            .arg(value.climbCount)
            .arg(value.jumpCount)
            .arg(value.descentCount));
    loadValue->setText(QStringLiteral("%1 pts")
            .arg(value.estimatedLoadPoints, 0, 'f', 1));
    loadDeviationValue->setText(percentText(value.loadDeviationPercent));
    workDeviationValue->setText(
            percentText(value.workDurationDeviationPercent));
    recoveryDeviationValue->setText(
            percentText(value.recoveryDurationDeviationPercent));
    totalDeviationValue->setText(
            percentText(value.totalDurationDeviationPercent));
    keyEffortRetentionValue->setText(QStringLiteral("%1/%2")
            .arg(value.preservedKeyEffortCount).arg(value.keyEffortCount));
    recoveryRetentionValue->setText(QStringLiteral("%1/%2")
            .arg(value.preservedRecoveryCount).arg(value.recoveryCount));
    terrainSignatureValue->setText(terrainSignatureText(previewResult));
    technicalExposureValue->setText(technicalExposureText(value));
    featureDensityValue->setText(QStringLiteral("%1/10 sections")
            .arg(value.technicalFeatureDensityPerTenSections, 0, 'f', 1));
    curvatureValue->setText(QStringLiteral("%1 deg/100 m")
            .arg(value.curvatureDegreesPer100m, 0, 'f', 1));
    prescriptionChangesValue->setText(prescriptionChangesText(value));
    workoutFirstComparisonValue->setText(comparisonText(modePreviews[0]));
    balancedComparisonValue->setText(comparisonText(modePreviews[1]));
    rideFirstComparisonValue->setText(comparisonText(modePreviews[2]));
}

void WorkoutGameCourseConversionDialog::browseOutput()
{
    const QString selected = QFileDialog::getSaveFileName(
            this, tr("Create MTB Course"), outputPathEdit->text(),
            tr("GoldenCheetah Course (*.crs)"));
    if (!selected.isEmpty()) outputPathEdit->setText(selected);
}

void WorkoutGameCourseConversionDialog::createCourse()
{
    showError({});
    if (previewResult.status != WorkoutGameCourseSourceStatus::Ready) {
        showError(tr("Check the course title and workout settings."));
        return;
    }
    WorkoutGameCourseDocument document = previewResult.document;
    document.title = titleEdit->text().trimmed();
    if (!WorkoutGameCourseDocumentCodec::valid(document)) {
        showError(tr("Check the course title and workout settings."));
        return;
    }

    QString path = outputPathEdit->text().trimmed();
    if (path.isEmpty()) {
        showError(tr("Choose a course file."));
        return;
    }
    if (!path.endsWith(QStringLiteral(".crs"), Qt::CaseInsensitive)) {
        path += QStringLiteral(".crs");
        outputPathEdit->setText(path);
    }
    QString error;
    const WorkoutGameCourseDocumentStatus status = editMode
            ? WorkoutGameCourseDocumentStore::replaceArtifact(
                path, document, error)
            : WorkoutGameCourseDocumentStore::saveNewArtifact(
                path, document, error);
    if (status != WorkoutGameCourseDocumentStatus::Ready) {
        showError(error.isEmpty() ? tr("Could not create the MTB course.") : error);
        return;
    }
    createdPath = QFileInfo(path).absoluteFilePath();
    accept();
}

void WorkoutGameCourseConversionDialog::showError(const QString &message)
{
    errorLabel->setText(message);
    errorLabel->setVisible(!message.isEmpty());
}
