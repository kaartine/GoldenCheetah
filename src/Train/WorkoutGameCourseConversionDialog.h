/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameCourseConversionDialog_h
#define _GC_WorkoutGameCourseConversionDialog_h

#include "WorkoutGameCourseSourceAdapter.h"

#include <QDialog>

#include <array>

class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;
class WorkoutGameCoursePreviewWidget;

class WorkoutGameCourseConversionDialog : public QDialog
{
    Q_OBJECT

public:
    WorkoutGameCourseConversionDialog(
            const WorkoutGameCourseSourceRequest &request,
            const QString &defaultCoursePath,
            QWidget *parent = nullptr);
    WorkoutGameCourseConversionDialog(
            const WorkoutGameCourseDocument &document,
            const QString &coursePath,
            QWidget *parent = nullptr);

    WorkoutGameCoursePreset selectedPreset() const;
    const WorkoutGameCourseSourceResult &currentResult() const;
    QString generatedCoursePath() const;

private slots:
    void selectWorkoutFirst();
    void selectBalanced();
    void selectRideFirst();
    void browseOutput();
    void createCourse();

private:
    void selectPreset(WorkoutGameCoursePreset preset);
    void generatePreviews();
    void refreshSummary();
    void showError(const QString &message);

    WorkoutGameCourseSourceRequest sourceRequest;
    WorkoutGameCourseDocument editSourceDocument;
    bool editMode = false;
    WorkoutGameCourseSourceResult previewResult;
    std::array<WorkoutGameCourseSourceResult, 3> modePreviews;
    WorkoutGameCoursePreset preset = WorkoutGameCoursePreset::Balanced;
    QString createdPath;

    QToolButton *workoutFirstButton = nullptr;
    QToolButton *balancedButton = nullptr;
    QToolButton *rideFirstButton = nullptr;
    QLabel *presetDescriptionLabel = nullptr;
    WorkoutGameCoursePreviewWidget *preview = nullptr;
    QLabel *durationValue = nullptr;
    QLabel *etaValue = nullptr;
    QLabel *distanceValue = nullptr;
    QLabel *ascentValue = nullptr;
    QLabel *featuresValue = nullptr;
    QLabel *loadValue = nullptr;
    QLabel *loadDeviationValue = nullptr;
    QLabel *workDeviationValue = nullptr;
    QLabel *recoveryDeviationValue = nullptr;
    QLabel *totalDeviationValue = nullptr;
    QLabel *keyEffortRetentionValue = nullptr;
    QLabel *recoveryRetentionValue = nullptr;
    QLabel *terrainSignatureValue = nullptr;
    QLabel *technicalExposureValue = nullptr;
    QLabel *featureDensityValue = nullptr;
    QLabel *runtimeExposureValue = nullptr;
    QLabel *prescriptionChangesValue = nullptr;
    QLabel *workoutFirstComparisonValue = nullptr;
    QLabel *balancedComparisonValue = nullptr;
    QLabel *rideFirstComparisonValue = nullptr;
    QLineEdit *titleEdit = nullptr;
    QLineEdit *outputPathEdit = nullptr;
    QLabel *errorLabel = nullptr;
    QPushButton *createButton = nullptr;
};

#endif
