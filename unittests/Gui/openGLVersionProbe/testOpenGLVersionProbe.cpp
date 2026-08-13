/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "OpenGLVersionProbe.h"

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QTest>

class TestOpenGLVersionProbe : public QObject
{
    Q_OBJECT

private slots:
    void nullSurfaceIsRejected();
    void invalidContextIsRejected();
    void automaticProbeDoesNotLeaveAContextCurrent();
};

void TestOpenGLVersionProbe::nullSurfaceIsRejected()
{
    QOpenGLContext context;

    QVERIFY(OpenGLVersionProbe::versionFromContext(
                context, nullptr).isEmpty());
    QVERIFY(QOpenGLContext::currentContext() != &context);
}

void TestOpenGLVersionProbe::invalidContextIsRejected()
{
    QOpenGLContext context;
    QOffscreenSurface surface;

    QVERIFY(OpenGLVersionProbe::versionFromContext(
                context, &surface).isEmpty());
    QVERIFY(QOpenGLContext::currentContext() != &context);
}

void TestOpenGLVersionProbe::automaticProbeDoesNotLeaveAContextCurrent()
{
    QVERIFY(!QOpenGLContext::currentContext());

    OpenGLVersionProbe::detect();

    QVERIFY(!QOpenGLContext::currentContext());
}

QTEST_MAIN(TestOpenGLVersionProbe)

#include "testOpenGLVersionProbe.moc"
