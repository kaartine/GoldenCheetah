/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

// Actual production-chart destruction regression; see README.md.
#include <QtTest>
#include <QPointer>
#include <QPropertyAnimation>
#include <QTimer>
#include <memory>

#include "AllPlot.h"
#include "Context.h"
#include "Settings.h"
#include "FilterEditor.h"

// Access only; construction and destruction are the real production methods.
class InspectableAllPlot final : public AllPlot
{
public:
    using AllPlot::AllPlot;
    AllPlotObject *ownedCurves() const { return standard; }
};

class CountedSeries final : public QwtSeriesData<QPointF>
{
public:
    explicit CountedSeries(int &destructions) : destructions(destructions) {}
    ~CountedSeries() override { ++destructions; }
    size_t size() const override { return 0; }
    QPointF sample(size_t) const override { return {}; }
    QRectF boundingRect() const override { return {}; }
private:
    int &destructions;
};

class CountedMarker final : public QwtPlotMarker
{
public:
    explicit CountedMarker(int &destructions) : destructions(destructions) {}
    ~CountedMarker() override { ++destructions; }
private:
    int &destructions;
};

class TestChartOwnership : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        // The production settings singleton is constructed before main(). The
        // launcher must isolate XDG paths before starting this executable.
        QVERIFY2(!qEnvironmentVariableIsEmpty("GC_CHART_OWNERSHIP_ROOT"),
                 "Use the isolated launcher documented in README.md");
        const QString root = qEnvironmentVariable("GC_CHART_OWNERSHIP_ROOT");
        QCOMPARE(qEnvironmentVariable("XDG_CONFIG_HOME"), root + "/config");
        QVERIFY(appsettings);
        GCColor::setupColors();
    }

    void allPlotDeletesHelper()
    {
        Context context(nullptr);
        auto plot = std::make_unique<AllPlot>(nullptr, nullptr, &context);
        QPointer<CurveColors> colors(plot->curveColors);
        QVERIFY(colors);
        plot.reset();
        QVERIFY2(colors.isNull(), "AllPlot must delete its CurveColors helper");
    }

    void allPlotDeletesIntervalSeries()
    {
        Context context(nullptr);
        int highlighterDestructions = 0;
        int hoverDestructions = 0;
        auto plot = std::make_unique<InspectableAllPlot>(nullptr, nullptr, &context);
        QVERIFY(plot->ownedCurves());
        // Install probes in the two actual, production-created curves. Their
        // destruction must delete their owned series; no surrogate owner is
        // involved. Replacing samples also releases the original series.
        plot->ownedCurves()->intervalHighlighterCurve->setSamples(
            new CountedSeries(highlighterDestructions));
        plot->ownedCurves()->intervalHoverCurve->setSamples(
            new CountedSeries(hoverDestructions));
        QCOMPARE(highlighterDestructions, 0);
        QCOMPARE(hoverDestructions, 0);
        // Intentionally unshown: this fixture does not provide an athlete,
        // AllPlotWindow or ride for drawing/interval-selection operations.
        plot.reset();
        QCOMPARE(highlighterDestructions, 1);
        QCOMPARE(hoverDestructions, 1);
    }

    void chartWindowDeletesAnimationsAndTimer()
    {
        Context context(nullptr);
        auto chart = std::make_unique<GcChartWindow>(&context);
        const auto animations = chart->findChildren<QPropertyAnimation *>(
            QString(), Qt::FindDirectChildrenOnly);
        const auto timers = chart->findChildren<QTimer *>(
            QString(), Qt::FindDirectChildrenOnly);
        QCOMPARE(animations.size(), 2);
        QCOMPARE(timers.size(), 1);
        QPointer<QPropertyAnimation> reveal(animations.at(0));
        QPointer<QPropertyAnimation> unreveal(animations.at(1));
        QPointer<QTimer> timer(timers.at(0));
        chart.reset();
        QVERIFY(reveal.isNull());
        QVERIFY(unreveal.isNull());
        QVERIFY(timer.isNull());
    }

    void allPlotDeletesSelectionMarkers()
    {
        Context context(nullptr);
        int firstDestructions = 0;
        int secondDestructions = 0;
        auto plot = std::make_unique<InspectableAllPlot>(nullptr, nullptr, &context);
        auto *curves = plot->ownedCurves();
        QVERIFY(curves);
        // Assign before reading: these fields were uninitialized in the old
        // implementation used for the differential regression check.
        curves->allMarker1 = new CountedMarker(firstDestructions);
        curves->allMarker2 = new CountedMarker(secondDestructions);
        curves->allMarker1->attach(plot.get());
        curves->allMarker2->attach(plot.get());
        plot.reset();
        QCOMPARE(firstDestructions, 1);
        QCOMPARE(secondDestructions, 1);
    }

    void filterEditorReusesAndDeletesCompletionObjects()
    {
        auto editor = std::make_unique<FilterEditor>();
        QSignalSpy textChanges(editor.get(), &QLineEdit::textChanged);
        editor->setFilterCommands({"distance", "power"});
        QPointer<QCompleter> completer(editor->completer());
        QVERIFY(completer);
        QPointer<QStringListModel> model(
            qobject_cast<QStringListModel *>(completer->model()));
        QVERIFY(model);
        QCOMPARE(completer->parent(), editor.get());
        QCOMPARE(model->parent(), editor.get());

        const QStringList replacement {"cadence", "speed"};
        completer->setCompletionPrefix("ca");
        editor->setFilterCommands(replacement);
        editor->setFilterCommands(replacement);
        QCOMPARE(editor->completer(), completer.data());
        QCOMPARE(editor->completer()->model(), model.data());
        QCOMPARE(completer->completionPrefix(), QString("ca"));
        QCOMPARE(model->stringList(), replacement);
        QCOMPARE(textChanges.count(), 0);
        QSignalSpy modelResets(model.data(), &QAbstractItemModel::modelReset);
        editor->setText("cad");
        QCOMPARE(textChanges.count(), 1);
        QCOMPARE(modelResets.count(), 1);
        editor.reset();
        QVERIFY(completer.isNull());
        QVERIFY(model.isNull());
    }

};

QTEST_MAIN(TestChartOwnership)
#include "testChartOwnership.moc"
