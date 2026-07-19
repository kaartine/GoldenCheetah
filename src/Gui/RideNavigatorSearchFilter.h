/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RIDENAVIGATORSEARCHFILTER_H
#define GC_RIDENAVIGATORSEARCHFILTER_H

#include <QAbstractItemModel>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QStringList>

enum class RideNavFilter { ALL=0, COMPLETED, PLANNED };

class SearchFilter : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit SearchFilter(QObject *parent = nullptr)
        : QSortFilterProxyModel(parent)
    {
    }

    void setSourceModel(QAbstractItemModel *model) override
    {
        QAbstractProxyModel::setSourceModel(model);
        model_ = model;

        fileIndex_ = -1;
        plannedIndex_ = -1;
        dirtyIndex_ = -1;
        for (int column = 0; column < model->columnCount(); ++column) {
            const QString header = model->headerData(
                column, Qt::Horizontal, Qt::DisplayRole).toString();
            if (header == QStringLiteral("filename") || header == tr("File")) {
                fileIndex_ = column;
            }
            if (header == QStringLiteral("planned")) plannedIndex_ = column;
            if (header == QStringLiteral("dirty")) dirtyIndex_ = column;
        }

        connect(model, SIGNAL(modelReset()), this, SIGNAL(modelReset()));
        connect(model, SIGNAL(dataChanged(QModelIndex, QModelIndex)),
                this, SIGNAL(dataChanged(QModelIndex, QModelIndex)));
        connect(model, SIGNAL(rowsInserted(QModelIndex,int,int)),
                this, SIGNAL(modelReset()));
        connect(model, SIGNAL(rowsMoved(QModelIndex,int,int,QModelIndex,int)),
                this, SIGNAL(modelReset()));
        connect(model, SIGNAL(rowsRemoved(QModelIndex,int,int)),
                this, SIGNAL(modelReset()));
    }

    bool filterAcceptsRow(
        int sourceRow,
        const QModelIndex &sourceParent) const override
    {
        if (fileIndex_ == -1 || plannedIndex_ == -1) return true;

        bool keyFound = true;
        bool display = true;
        const QModelIndex plannedIndex = model_->index(
            sourceRow, plannedIndex_, sourceParent);

        if (plannedIndex.isValid()) {
            if (model_->data(plannedIndex, Qt::DisplayRole).toBool()) {
                if (rideNavFilter_ == RideNavFilter::COMPLETED) display = false;
            } else if (rideNavFilter_ == RideNavFilter::PLANNED) {
                display = false;
            }
        }

        if (searchActive_) {
            const QModelIndex sourceIndex = model_->index(
                sourceRow, fileIndex_, sourceParent);
            if (sourceIndex.isValid()) {
                const QString key = model_->data(
                    sourceIndex, Qt::DisplayRole).toString();
                keyFound = strings_.contains(key);
            }
        }

        return keyFound && display;
    }

public slots:
    void setDisplayFilter(RideNavFilter filter)
    {
        beginResetModel();
        rideNavFilter_ = filter;
        endResetModel();
    }

    void setStrings(QStringList list)
    {
        beginResetModel();
        strings_.clear();
        strings_.reserve(list.size());
        for (const QString &string : list) strings_.insert(string);
        searchActive_ = true;
        endResetModel();
    }

    void clearStrings()
    {
        beginResetModel();
        strings_.clear();
        searchActive_ = false;
        endResetModel();
    }

private:
    QAbstractItemModel *model_ = nullptr;
    QSet<QString> strings_;
    int fileIndex_ = -1;
    int plannedIndex_ = -1;
    int dirtyIndex_ = -1;
    bool searchActive_ = false;
    RideNavFilter rideNavFilter_ = RideNavFilter::ALL;
};

#endif // GC_RIDENAVIGATORSEARCHFILTER_H
