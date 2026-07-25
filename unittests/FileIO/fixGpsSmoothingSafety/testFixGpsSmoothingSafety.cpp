/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <QtTest>

#include "FileIO/FixGPSSmoothingSafety.h"

#include <string>
#include <vector>

using FixGPSSmoothingSafety::SourceIndexes;

class TestFixGpsSmoothingSafety : public QObject
{
    Q_OBJECT

private slots:
    void gatherPreservesFilteredSourceIndexes();
    void gatherRejectsEmptyResult();
    void mappedGatherValidatesItsInput();
    void mappedGatherPreservesFurtherFiltering();
    void applyUsesSourceIndexes();
    void applyRejectsInvalidMapsBeforeWriting();
    void splineRequiresCubicControlSet();
    void controlOutputCardinalityMustMatch();
};

void TestFixGpsSmoothingSafety::gatherPreservesFilteredSourceIndexes()
{
    std::vector<int> controls;
    SourceIndexes sourceIndexes;

    const bool gathered = FixGPSSmoothingSafety::gatherControls<int>(
        5, controls, sourceIndexes,
        [](std::size_t sourceIndex, int &control) {
            if ((sourceIndex % 2) == 0) {
                return false;
            }
            control = static_cast<int>(sourceIndex * 10);
            return true;
        });

    QVERIFY(gathered);
    QCOMPARE(controls, std::vector<int>({10, 30}));
    QCOMPARE(sourceIndexes, SourceIndexes({1, 3}));
}

void TestFixGpsSmoothingSafety::gatherRejectsEmptyResult()
{
    std::vector<int> controls({99});
    SourceIndexes sourceIndexes({99});

    const bool gathered = FixGPSSmoothingSafety::gatherControls<int>(
        4, controls, sourceIndexes,
        [](std::size_t, int &) {
            return false;
        });

    QVERIFY(!gathered);
    QVERIFY(controls.empty());
    QVERIFY(sourceIndexes.empty());
}

void TestFixGpsSmoothingSafety::mappedGatherValidatesItsInput()
{
    const std::vector<int> inputs({10, 20});
    std::vector<std::string> outputs({"stale"});
    SourceIndexes outputIndexes({99});

    const auto builder = [](std::size_t, int input, std::string &output) {
        output = std::to_string(input);
        return true;
    };

    QVERIFY(!FixGPSSmoothingSafety::gatherMappedControls(
        5, SourceIndexes({1}), inputs, outputs, outputIndexes, builder));
    QVERIFY(outputs.empty());
    QVERIFY(outputIndexes.empty());

    QVERIFY(!FixGPSSmoothingSafety::gatherMappedControls(
        5, SourceIndexes({1, 5}), inputs, outputs, outputIndexes, builder));
    QVERIFY(!FixGPSSmoothingSafety::gatherMappedControls(
        5, SourceIndexes({3, 2}), inputs, outputs, outputIndexes, builder));
    QVERIFY(!FixGPSSmoothingSafety::gatherMappedControls(
        5, SourceIndexes({2, 2}), inputs, outputs, outputIndexes, builder));
}

void TestFixGpsSmoothingSafety::mappedGatherPreservesFurtherFiltering()
{
    const std::vector<int> inputs({10, 20, 30});
    const SourceIndexes inputIndexes({0, 2, 4});
    std::vector<std::string> outputs;
    SourceIndexes outputIndexes;

    const bool gathered = FixGPSSmoothingSafety::gatherMappedControls(
        5, inputIndexes, inputs, outputs, outputIndexes,
        [](std::size_t sourceIndex, int input, std::string &output) {
            if (sourceIndex == 2) {
                return false;
            }
            output = std::to_string(input);
            return true;
        });

    QVERIFY(gathered);
    QCOMPARE(outputs, std::vector<std::string>({"10", "30"}));
    QCOMPARE(outputIndexes, SourceIndexes({0, 4}));
}

void TestFixGpsSmoothingSafety::applyUsesSourceIndexes()
{
    const std::vector<std::string> outputs({"first", "second"});
    const SourceIndexes sourceIndexes({1, 3});
    std::vector<std::string> destination(5, "unchanged");
    int calls = 0;

    const bool applied = FixGPSSmoothingSafety::applyMappedOutputs(
        destination.size(), sourceIndexes, outputs,
        [&destination, &calls](std::size_t sourceIndex,
                               const std::string &output) {
            destination[sourceIndex] = output;
            ++calls;
        });

    QVERIFY(applied);
    QCOMPARE(calls, 2);
    QCOMPARE(destination, std::vector<std::string>(
        {"unchanged", "first", "unchanged", "second", "unchanged"}));
}

void TestFixGpsSmoothingSafety::applyRejectsInvalidMapsBeforeWriting()
{
    const std::vector<int> outputs({10, 20});
    int calls = 0;
    const auto apply = [&calls](std::size_t, int) {
        ++calls;
    };

    QVERIFY(!FixGPSSmoothingSafety::applyMappedOutputs(
        5, SourceIndexes({1}), outputs, apply));
    QVERIFY(!FixGPSSmoothingSafety::applyMappedOutputs(
        5, SourceIndexes({1, 5}), outputs, apply));
    QVERIFY(!FixGPSSmoothingSafety::applyMappedOutputs(
        5, SourceIndexes({3, 2}), outputs, apply));
    QCOMPARE(calls, 0);
}

void TestFixGpsSmoothingSafety::splineRequiresCubicControlSet()
{
    for (std::size_t count = 0; count < 4; ++count) {
        QVERIFY(!FixGPSSmoothingSafety::hasUsableSplineInput(count, 3));
    }

    QVERIFY(!FixGPSSmoothingSafety::hasUsableSplineInput(4, 2));
    QVERIFY(FixGPSSmoothingSafety::hasUsableSplineInput(4, 3));
    QVERIFY(FixGPSSmoothingSafety::hasUsableSplineInput(100, 99));
}

void TestFixGpsSmoothingSafety::controlOutputCardinalityMustMatch()
{
    QVERIFY(!FixGPSSmoothingSafety::hasAlignedControlOutput(0, 0));
    QVERIFY(!FixGPSSmoothingSafety::hasAlignedControlOutput(4, 0));
    QVERIFY(!FixGPSSmoothingSafety::hasAlignedControlOutput(4, 3));
    QVERIFY(!FixGPSSmoothingSafety::hasAlignedControlOutput(4, 5));
    QVERIFY(FixGPSSmoothingSafety::hasAlignedControlOutput(4, 4));
}

QTEST_APPLESS_MAIN(TestFixGpsSmoothingSafety)

#include "testFixGpsSmoothingSafety.moc"
