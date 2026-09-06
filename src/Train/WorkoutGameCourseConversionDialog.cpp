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
#include "WorkoutGameCoursePreviewMetrics.h"
#include "WorkoutGameRoadPlan.h"

#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
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
    label->setWordWrap(true);
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
    WorkoutGameCoursePreviewRoadMetrics roadMetrics;
    if (result.document.course.roadPlan) {
        roadMetrics = WorkoutGameCoursePreviewMetrics::roadMetrics(
                *result.document.course.roadPlan);
    }
    return QObject::tr(
            "grade %1 | +%2 m | %3 technical / %4 sections | "
            "%5 curve events")
            .arg(result.document.generationParameters.gradeScale, 0, 'f', 2)
            .arg(std::lround(result.summary.elevationGainMeters))
            .arg(result.summary.technicalFeatureCount)
            .arg(qulonglong(result.document.course.sections.size()))
            .arg(roadMetrics.curveEventCount);
}

QString technicalExposureText(
        const WorkoutGameCourseConversionSummary &summary)
{
    return summary.technicalTerrainExposureApplicable
            ? percentText(summary.technicalTerrainExposurePercent)
            : QObject::tr("N/A");
}

QString presetDescriptionText(WorkoutGameCoursePreset preset)
{
    switch (preset) {
    case WorkoutGameCoursePreset::WorkoutFirst:
        return QObject::tr(
            "Calm training trail keeps attention on the workout with gentler "
            "terrain and fewer technical sections. All prescribed power "
            "targets and timings stay unchanged.");
    case WorkoutGameCoursePreset::Balanced:
        return QObject::tr(
            "Varied training trail balances workout focus with more varied "
            "terrain and technical sections. All prescribed power targets "
            "and timings stay unchanged.");
    case WorkoutGameCoursePreset::RideFirst:
        return QObject::tr(
            "Technical game trail adds the densest features and sharper curves "
            "while all prescribed power targets and timings stay unchanged.");
    }
    return {};
}

QString presetMetricsText(WorkoutGameCoursePreset preset)
{
    switch (preset) {
    case WorkoutGameCoursePreset::WorkoutFirst:
        return QObject::tr(
            "Generation details: grade 0.82x | curvature 1.00x | "
            "2-4 technical segments per 10 eligible segments");
    case WorkoutGameCoursePreset::Balanced:
        return QObject::tr(
            "Generation details: grade 1.00x | curvature 1.30x | "
            "5-7 technical segments per 10 eligible segments");
    case WorkoutGameCoursePreset::RideFirst:
        return QObject::tr(
            "Generation details: grade 1.18x | curvature 2.60x | "
            "8-10 technical segments per 10 eligible segments");
    }
    return {};
}

QString runtimeExposureText(const WorkoutGameCourseSourceResult &result)
{
    if (result.status != WorkoutGameCourseSourceStatus::Ready) return {};
    return QObject::tr("100% of prescribed interval time");
}

QString prescriptionChangesText(
        const WorkoutGameCourseConversionSummary &summary)
{
    if (summary.prescriptionChanges.empty()) {
        return QObject::tr("No prescription changes");
    }
    QStringList values;
    for (const auto &change : summary.prescriptionChanges) {
        QString role;
        switch (change.role) {
        case WorkoutGameCourseIntervalRole::Prescribed:
            role = QObject::tr("prescribed"); break;
        case WorkoutGameCourseIntervalRole::NonPrescriptiveWarmup:
            role = QObject::tr("non-prescriptive warmup"); break;
        case WorkoutGameCourseIntervalRole::NonPrescriptiveCooldown:
            role = QObject::tr("non-prescriptive cooldown"); break;
        case WorkoutGameCourseIntervalRole::NonPrescriptiveTransition:
            role = QObject::tr("non-prescriptive transition"); break;
        }
        values.append(QObject::tr("#%1 %2: %3 ms")
                .arg(qulonglong(change.intervalIndex + 1))
                .arg(role)
                .arg(change.generatedDurationMs - change.sourceDurationMs));
    }
    return values.join(QStringLiteral("; "));
}

QString comparisonText(const WorkoutGameCourseSourceResult &result)
{
    if (result.status != WorkoutGameCourseSourceStatus::Ready) {
        return QObject::tr("Unavailable");
    }
    const WorkoutGameCourseConversionSummary &value = result.summary;
    return QObject::tr(
            "%1 | %2 km | +%3 m | technical %4 | "
            "key efforts %5/%6 | prescribed recoveries %7/%8")
            .arg(durationText(value.nominalDurationMs))
            .arg(value.distanceMeters / 1000.0, 0, 'f', 1)
            .arg(std::lround(value.elevationGainMeters))
            .arg(technicalExposureText(value))
            .arg(value.preservedKeyEffortCount)
            .arg(value.keyEffortCount)
            .arg(value.preservedRecoveryCount)
            .arg(value.recoveryCount);
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

    QLabel *prescriptionGuarantee = new QLabel(tr(
            "All presets preserve prescribed targets and timing. "
            "Choose the trail character."), this);
    prescriptionGuarantee->setObjectName(
            QStringLiteral("prescriptionGuaranteeLabel"));
    prescriptionGuarantee->setWordWrap(true);
    layout->addWidget(prescriptionGuarantee);

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
            tr("Calm training trail"), "workoutFirstPresetButton");
    balancedButton = addPreset(
            tr("Varied training trail"), "balancedPresetButton");
    rideFirstButton = addPreset(
            tr("Technical game trail"), "rideFirstPresetButton");
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

    QScrollArea *detailsScrollArea = new QScrollArea(this);
    detailsScrollArea->setObjectName(
            QStringLiteral("courseDetailsScrollArea"));
    detailsScrollArea->setWidgetResizable(true);
    detailsScrollArea->setFrameShape(QFrame::NoFrame);
    detailsScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    QWidget *details = new QWidget(detailsScrollArea);
    details->setObjectName(QStringLiteral("courseDetailsScrollContents"));
    details->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    QVBoxLayout *detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(0, 0, 8, 0);
    detailsLayout->setSpacing(14);
    detailsLayout->setSizeConstraint(QLayout::SetMinAndMaxSize);

    presetDescriptionLabel = new QLabel(details);
    presetDescriptionLabel->setObjectName(
            QStringLiteral("presetDescriptionLabel"));
    presetDescriptionLabel->setWordWrap(true);
    detailsLayout->addWidget(presetDescriptionLabel);

    presetMetricsLabel = new QLabel(details);
    presetMetricsLabel->setObjectName(QStringLiteral("presetMetricsLabel"));
    presetMetricsLabel->setWordWrap(true);
    detailsLayout->addWidget(presetMetricsLabel);

    QWidget *modeComparison = new QWidget(details);
    modeComparison->setObjectName(QStringLiteral("courseModeComparison"));
    QGridLayout *comparison = new QGridLayout(modeComparison);
    comparison->setContentsMargins(0, 0, 0, 0);
    comparison->setHorizontalSpacing(12);
    comparison->setVerticalSpacing(4);
    workoutFirstComparisonValue = summaryValue(
            "workoutFirstComparisonValue", modeComparison);
    balancedComparisonValue = summaryValue(
            "balancedComparisonValue", modeComparison);
    rideFirstComparisonValue = summaryValue(
            "rideFirstComparisonValue", modeComparison);
    comparison->addWidget(new QLabel(tr("Calm"), modeComparison), 0, 0);
    comparison->addWidget(workoutFirstComparisonValue, 0, 1);
    comparison->addWidget(new QLabel(tr("Varied"), modeComparison), 1, 0);
    comparison->addWidget(balancedComparisonValue, 1, 1);
    comparison->addWidget(new QLabel(tr("Technical"), modeComparison), 2, 0);
    comparison->addWidget(rideFirstComparisonValue, 2, 1);
    comparison->setColumnStretch(1, 1);
    detailsLayout->addWidget(modeComparison);

    preview = new WorkoutGameCoursePreviewWidget(details);
    detailsLayout->addWidget(preview);

    QGridLayout *summary = new QGridLayout;
    summary->setHorizontalSpacing(18);
    summary->setVerticalSpacing(5);
    durationValue = summaryValue("durationValue", details);
    etaValue = summaryValue("etaValue", details);
    distanceValue = summaryValue("distanceValue", details);
    ascentValue = summaryValue("ascentValue", details);
    featuresValue = summaryValue("featuresValue", details);
    loadValue = summaryValue("loadValue", details);
    loadDeviationValue = summaryValue("loadDeviationValue", details);
    workDeviationValue = summaryValue("workDeviationValue", details);
    recoveryDeviationValue = summaryValue("recoveryDeviationValue", details);
    totalDeviationValue = summaryValue("totalDeviationValue", details);
    keyEffortRetentionValue = summaryValue("keyEffortRetentionValue", details);
    recoveryRetentionValue = summaryValue("recoveryRetentionValue", details);
    terrainSignatureValue = summaryValue("terrainSignatureValue", details);
    technicalExposureValue = summaryValue("technicalExposureValue", details);
    featureDensityValue = summaryValue("featureDensityValue", details);
    runtimeExposureValue = summaryValue("runtimeExposureValue", details);
    prescriptionChangesValue = summaryValue("prescriptionChangesValue", details);
    int summaryRow = 0;
    auto addSummaryRow = [&](const QString &name, QLabel *value) {
        QLabel *nameLabel = new QLabel(name, details);
        nameLabel->setWordWrap(true);
        summary->addWidget(nameLabel, summaryRow, 0);
        summary->addWidget(value, summaryRow, 1);
        ++summaryRow;
    };
    addSummaryRow(tr("Workout duration"), durationValue);
    addSummaryRow(tr("Expected ride time"), etaValue);
    addSummaryRow(tr("Distance"), distanceValue);
    addSummaryRow(tr("Ascent"), ascentValue);
    addSummaryRow(tr("Features"), featuresValue);
    addSummaryRow(tr("Load"), loadValue);
    addSummaryRow(tr("Load deviation"), loadDeviationValue);
    addSummaryRow(tr("Hard segments preserved"), keyEffortRetentionValue);
    addSummaryRow(tr("Easy segments preserved"), recoveryRetentionValue);
    addSummaryRow(tr("Work deviation"), workDeviationValue);
    addSummaryRow(tr("Recovery/rest deviation"), recoveryDeviationValue);
    addSummaryRow(tr("Total deviation"), totalDeviationValue);
    addSummaryRow(tr("Terrain signature"), terrainSignatureValue);
    addSummaryRow(tr("Technical exposure"), technicalExposureValue);
    addSummaryRow(tr("Feature density"), featureDensityValue);
    addSummaryRow(tr("Minimum section time"), runtimeExposureValue);
    addSummaryRow(tr("Prescription changes"), prescriptionChangesValue);
    summary->setColumnStretch(1, 1);
    detailsLayout->addLayout(summary);

    detailsScrollArea->setWidget(details);
    layout->addWidget(detailsScrollArea, 1);

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
    buttons->setObjectName(QStringLiteral("courseDialogButtonBox"));
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
    presetDescriptionLabel->setText(presetDescriptionText(preset));
    presetMetricsLabel->setText(presetMetricsText(preset));
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
        runtimeExposureValue->clear();
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
    runtimeExposureValue->setText(runtimeExposureText(previewResult));
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
