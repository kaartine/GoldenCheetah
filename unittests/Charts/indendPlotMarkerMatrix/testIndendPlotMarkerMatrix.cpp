/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <QtTest>

#include "Charts/IndendPlotMarker.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <new>

class TestIndendPlotMarkerMatrix : public QObject
{
    Q_OBJECT

private slots:
    void constructorInitializesCanvasIdentity();
    void copyDoesNotReadDestinationStorage();
    void copyOwnsIndependentPixels();
    void assignmentOwnsIndependentPixels();
    void setUsesColumnStride();
};

void TestIndendPlotMarkerMatrix::constructorInitializesCanvasIdentity()
{
    using Matrix = QwtIndPlotMarker::Matrix;
    alignas(Matrix) std::array<std::byte, sizeof(Matrix)> storage;
    std::memset(storage.data(), 0xa5, storage.size());

    Matrix *matrix = new (storage.data()) Matrix(2, 3);
    QCOMPARE(matrix->rows(), 2UL);
    QCOMPARE(matrix->cols(), 3UL);
    QCOMPARE(matrix->canvasId(), uintptr_t(0));
    matrix->~Matrix();
}

void TestIndendPlotMarkerMatrix::copyDoesNotReadDestinationStorage()
{
    using Matrix = QwtIndPlotMarker::Matrix;
    Matrix source(2, 3);
    source.setCanvasId(uintptr_t(0x1234));
    source.drawnAt(1, 2, 1, 1);

    alignas(Matrix) std::array<std::byte, sizeof(Matrix)> storage;
    std::memset(storage.data(), 0xa5, storage.size());

    Matrix *copy = new (storage.data()) Matrix(source);
    QCOMPARE(copy->rows(), 2UL);
    QCOMPARE(copy->cols(), 3UL);
    QCOMPARE(copy->canvasId(), uintptr_t(0x1234));
    QVERIFY((*copy)(1, 2));
    copy->~Matrix();
}

void TestIndendPlotMarkerMatrix::copyOwnsIndependentPixels()
{
    using Matrix = QwtIndPlotMarker::Matrix;
    Matrix source(2, 3);
    source.drawnAt(0, 1, 1, 1);
    Matrix copy(source);

    source.init();

    QVERIFY(!source(0, 1));
    QVERIFY(copy(0, 1));
}

void TestIndendPlotMarkerMatrix::assignmentOwnsIndependentPixels()
{
    using Matrix = QwtIndPlotMarker::Matrix;
    Matrix source(2, 3);
    source.setCanvasId(uintptr_t(0x5678));
    source.drawnAt(1, 1, 1, 1);
    Matrix assigned(1, 1);

    assigned = source;
    assigned = assigned;
    source.init();
    source.setCanvasId(0);

    QCOMPARE(assigned.rows(), 2UL);
    QCOMPARE(assigned.cols(), 3UL);
    QCOMPARE(assigned.canvasId(), uintptr_t(0x5678));
    QVERIFY(assigned(1, 1));
}

void TestIndendPlotMarkerMatrix::setUsesColumnStride()
{
    QwtIndPlotMarker::Matrix matrix(2, 3);

    matrix.set(1, 2, true);

    QVERIFY(matrix(1, 2));
    QVERIFY(!matrix(1, 1));
}

QTEST_APPLESS_MAIN(TestIndendPlotMarkerMatrix)

#include "testIndendPlotMarkerMatrix.moc"
