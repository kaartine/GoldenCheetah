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
    resize(900, 680);
    setMinimumSize(720, 560);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(14);

    QLabel *heading = new QLabel(tr("Create MTB Course"), this);
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
    summary->setColumnStretch(1, 1);
    summary->setColumnStretch(3, 1);
    layout->addLayout(summary);

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

    selectPreset(WorkoutGameCoursePreset::Balanced);
    if (previewResult.status == WorkoutGameCourseSourceStatus::Ready) {
        titleEdit->setText(previewResult.document.title);
    }
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
    sourceRequest.preset = preset;
    previewResult = WorkoutGameCourseSourceAdapter::convert(sourceRequest);
    preview->setResult(previewResult);
    refreshSummary();
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
    featuresValue->setText(tr("%1 climbs | %2 jumps | %3 descents")
            .arg(value.climbCount)
            .arg(value.jumpCount)
            .arg(value.descentCount));
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
    sourceRequest.title = titleEdit->text().trimmed();
    sourceRequest.preset = preset;
    previewResult = WorkoutGameCourseSourceAdapter::convert(sourceRequest);
    if (previewResult.status != WorkoutGameCourseSourceStatus::Ready) {
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
    const WorkoutGameCourseDocumentStatus status =
            WorkoutGameCourseDocumentStore::saveNewArtifact(
                path, previewResult.document, error);
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
