/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "Bindings.h"

#include <QTest>

#include <memory>
#include <utility>

namespace {

void verifyDefaultState(const PythonDataSeries &series)
{
    QVERIFY(series.name.isEmpty());
    QCOMPARE(series.count, Py_ssize_t(0));
    QCOMPARE(series.data, nullptr);
    QVERIFY(series.readOnly);
    QCOMPARE(series.seriesType, int(RideFile::none));
    QCOMPARE(series.rideFile, nullptr);
}

} // namespace

class TestPythonDataSeriesOwnership : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void copiedSeriesOutlivesSource();
    void copyAssignmentOutlivesSource();
    void selfCopyAssignmentPreservesState();
    void moveConstructionTransfersOwnership();
    void moveAssignmentTransfersOwnership();
    void selfMoveAssignmentPreservesState();
    void pointerBridgeTransfersOwnership();
    void nullPointerBridgeCreatesDefaultState();
};

void TestPythonDataSeriesOwnership::copiedSeriesOutlivesSource()
{
    std::unique_ptr<PythonDataSeries> copy;
    QObject rideFileSentinel;
    RideFile *const rideFile = reinterpret_cast<RideFile *>(&rideFileSentinel);

    {
        PythonDataSeries source(QStringLiteral("watts"), 3, false, RideFile::watts, rideFile);
        source.data[0] = 175.0;
        source.data[1] = 250.5;
        source.data[2] = 325.0;

        copy.reset(new PythonDataSeries(source));
        QVERIFY(copy->data != source.data);
    }

    QCOMPARE(copy->name, QStringLiteral("watts"));
    QCOMPARE(copy->count, Py_ssize_t(3));
    QCOMPARE(copy->data[0], 175.0);
    QCOMPARE(copy->data[1], 250.5);
    QCOMPARE(copy->data[2], 325.0);
    QVERIFY(!copy->readOnly);
    QCOMPARE(copy->seriesType, int(RideFile::watts));
    QCOMPARE(copy->rideFile, rideFile);
}

void TestPythonDataSeriesOwnership::copyAssignmentOutlivesSource()
{
    PythonDataSeries copy(QStringLiteral("old"), 1);
    copy.data[0] = -1.0;
    QObject rideFileSentinel;
    RideFile *const rideFile = reinterpret_cast<RideFile *>(&rideFileSentinel);

    {
        PythonDataSeries source(QStringLiteral("cadence"), 2, false, RideFile::cad, rideFile);
        source.data[0] = 88.0;
        source.data[1] = 92.0;

        copy = source;
        QVERIFY(copy.data != source.data);
    }

    QCOMPARE(copy.name, QStringLiteral("cadence"));
    QCOMPARE(copy.count, Py_ssize_t(2));
    QCOMPARE(copy.data[0], 88.0);
    QCOMPARE(copy.data[1], 92.0);
    QVERIFY(!copy.readOnly);
    QCOMPARE(copy.seriesType, int(RideFile::cad));
    QCOMPARE(copy.rideFile, rideFile);
}

void TestPythonDataSeriesOwnership::selfCopyAssignmentPreservesState()
{
    QObject rideFileSentinel;
    RideFile *const rideFile = reinterpret_cast<RideFile *>(&rideFileSentinel);
    PythonDataSeries series(QStringLiteral("torque"), 2, false, RideFile::nm, rideFile);
    series.data[0] = 31.5;
    series.data[1] = 42.0;
    double *const allocation = series.data;

    series = series;

    QCOMPARE(series.name, QStringLiteral("torque"));
    QCOMPARE(series.count, Py_ssize_t(2));
    QCOMPARE(series.data, allocation);
    QCOMPARE(series.data[0], 31.5);
    QCOMPARE(series.data[1], 42.0);
    QVERIFY(!series.readOnly);
    QCOMPARE(series.seriesType, int(RideFile::nm));
    QCOMPARE(series.rideFile, rideFile);
}

void TestPythonDataSeriesOwnership::moveConstructionTransfersOwnership()
{
    QObject rideFileSentinel;
    RideFile *const rideFile = reinterpret_cast<RideFile *>(&rideFileSentinel);
    PythonDataSeries source(QStringLiteral("speed"), 2, false, RideFile::kph, rideFile);
    source.data[0] = 10.0;
    source.data[1] = 12.5;
    double *const allocation = source.data;

    PythonDataSeries moved(std::move(source));

    QCOMPARE(moved.data, allocation);
    QCOMPARE(moved.name, QStringLiteral("speed"));
    QCOMPARE(moved.count, Py_ssize_t(2));
    QCOMPARE(moved.data[0], 10.0);
    QCOMPARE(moved.data[1], 12.5);
    QVERIFY(!moved.readOnly);
    QCOMPARE(moved.seriesType, int(RideFile::kph));
    QCOMPARE(moved.rideFile, rideFile);
    verifyDefaultState(source);
}

void TestPythonDataSeriesOwnership::moveAssignmentTransfersOwnership()
{
    QObject rideFileSentinel;
    RideFile *const rideFile = reinterpret_cast<RideFile *>(&rideFileSentinel);
    PythonDataSeries source(QStringLiteral("distance"), 2, false, RideFile::km, rideFile);
    source.data[0] = 1.25;
    source.data[1] = 2.5;
    double *const allocation = source.data;

    PythonDataSeries moved(QStringLiteral("old"), 1);
    moved.data[0] = -1.0;
    moved = std::move(source);

    QCOMPARE(moved.data, allocation);
    QCOMPARE(moved.name, QStringLiteral("distance"));
    QCOMPARE(moved.count, Py_ssize_t(2));
    QCOMPARE(moved.data[0], 1.25);
    QCOMPARE(moved.data[1], 2.5);
    QVERIFY(!moved.readOnly);
    QCOMPARE(moved.seriesType, int(RideFile::km));
    QCOMPARE(moved.rideFile, rideFile);
    verifyDefaultState(source);
}

void TestPythonDataSeriesOwnership::selfMoveAssignmentPreservesState()
{
    QObject rideFileSentinel;
    RideFile *const rideFile = reinterpret_cast<RideFile *>(&rideFileSentinel);
    PythonDataSeries series(QStringLiteral("altitude"), 2, false, RideFile::alt, rideFile);
    series.data[0] = 240.0;
    series.data[1] = 245.5;
    double *const allocation = series.data;
    PythonDataSeries *const self = &series;

    series = std::move(*self);

    QCOMPARE(series.name, QStringLiteral("altitude"));
    QCOMPARE(series.count, Py_ssize_t(2));
    QCOMPARE(series.data, allocation);
    QCOMPARE(series.data[0], 240.0);
    QCOMPARE(series.data[1], 245.5);
    QVERIFY(!series.readOnly);
    QCOMPARE(series.seriesType, int(RideFile::alt));
    QCOMPARE(series.rideFile, rideFile);
}

void TestPythonDataSeriesOwnership::pointerBridgeTransfersOwnership()
{
    QObject rideFileSentinel;
    RideFile *const rideFile = reinterpret_cast<RideFile *>(&rideFileSentinel);
    PythonDataSeries *returned = new PythonDataSeries(
        QStringLiteral("heart rate"), 2, false, RideFile::hr, rideFile);
    returned->data[0] = 145.0;
    returned->data[1] = 151.0;
    double *const allocation = returned->data;

    PythonDataSeries wrapped(returned);

    QCOMPARE(wrapped.data, allocation);
    QCOMPARE(wrapped.name, QStringLiteral("heart rate"));
    QCOMPARE(wrapped.count, Py_ssize_t(2));
    QCOMPARE(wrapped.data[0], 145.0);
    QCOMPARE(wrapped.data[1], 151.0);
    QVERIFY(!wrapped.readOnly);
    QCOMPARE(wrapped.seriesType, int(RideFile::hr));
    QCOMPARE(wrapped.rideFile, rideFile);
}

void TestPythonDataSeriesOwnership::nullPointerBridgeCreatesDefaultState()
{
    PythonDataSeries wrapped(nullptr);

    verifyDefaultState(wrapped);
}

QTEST_APPLESS_MAIN(TestPythonDataSeriesOwnership)
#include "testPythonDataSeriesOwnership.moc"
