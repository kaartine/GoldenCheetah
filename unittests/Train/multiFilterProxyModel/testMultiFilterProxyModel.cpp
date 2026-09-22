/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Train/ModelFilter.h"
#include "Train/MultiFilterProxyModel.h"

#include <QStandardItemModel>
#include <QTest>

namespace {

enum Column
{
    Name,
    FullText,
    LastRun,
    ColumnCount
};

void appendWorkout(
        QStandardItemModel &model,
        const QString &name,
        const QString &fullText,
        qlonglong lastRun)
{
    QList<QStandardItem *> row;
    row << new QStandardItem(name)
        << new QStandardItem(fullText)
        << new QStandardItem;
    row[LastRun]->setData(lastRun, Qt::DisplayRole);
    model.appendRow(row);
}

void populateWorkoutModel(QStandardItemModel &model)
{
    model.setColumnCount(ColumnCount);
    appendWorkout(model, QStringLiteral("Easy spin"),
                  QStringLiteral("Easy spin recovery erg"), 100);
    appendWorkout(model, QStringLiteral("Hill repeats"),
                  QStringLiteral("Hill repeats hard climbing vo2"), 300);
    appendWorkout(model, QStringLiteral("Tempo trail"),
                  QStringLiteral("Tempo trail mtb balanced"), 200);
}

} // namespace

class TestMultiFilterProxyModel : public QObject
{
    Q_OBJECT

private slots:
    void plainSearchMatchesAllTermsCaseInsensitively()
    {
        QStandardItemModel source;
        populateWorkoutModel(source);
        MultiFilterProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setSearchColumn(FullText);

        proxy.setSearchText(QStringLiteral("  HILL   Vo2 "));

        QCOMPARE(proxy.searchText(), QStringLiteral("HILL Vo2"));
        QCOMPARE(proxy.rowCount(), 1);
        QCOMPARE(proxy.index(0, Name).data().toString(),
                 QStringLiteral("Hill repeats"));
    }

    void searchCombinesWithStructuredFilters()
    {
        QStandardItemModel source;
        populateWorkoutModel(source);
        MultiFilterProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setSearchColumn(FullText);
        proxy.setSearchText(QStringLiteral("trail"));
        proxy.setFilters({new ModelNumberRangeFilter(LastRun, 150, 250)});

        QCOMPARE(proxy.rowCount(), 1);
        QCOMPARE(proxy.index(0, Name).data().toString(),
                 QStringLiteral("Tempo trail"));

        proxy.setSearchText(QString());
        QCOMPARE(proxy.rowCount(), 1);
    }

    void recentlyUsedSortsDescendingWithUnusedLast()
    {
        QStandardItemModel source;
        populateWorkoutModel(source);
        MultiFilterProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.sort(LastRun, Qt::DescendingOrder);

        QCOMPARE(proxy.index(0, Name).data().toString(),
                 QStringLiteral("Hill repeats"));
        QCOMPARE(proxy.index(1, Name).data().toString(),
                 QStringLiteral("Tempo trail"));
        QCOMPARE(proxy.index(2, Name).data().toString(),
                 QStringLiteral("Easy spin"));
    }

    void invalidSearchColumnFailsClosed()
    {
        QStandardItemModel source;
        populateWorkoutModel(source);
        MultiFilterProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setSearchColumn(ColumnCount + 1);
        proxy.setSearchText(QStringLiteral("easy"));

        QCOMPARE(proxy.rowCount(), 0);
    }
};

QTEST_APPLESS_MAIN(TestMultiFilterProxyModel)
#include "testMultiFilterProxyModel.moc"
