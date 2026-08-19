/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _GC_WorkoutGameCoursePreviewWidget_h
#define _GC_WorkoutGameCoursePreviewWidget_h

#include "WorkoutGameCourseSourceAdapter.h"

#include <QWidget>

class WorkoutGameCoursePreviewWidget : public QWidget
{
public:
    explicit WorkoutGameCoursePreviewWidget(QWidget *parent = nullptr);

    void setResult(const WorkoutGameCourseSourceResult &result);
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    WorkoutGameCourseSourceResult currentResult;
};

#endif
